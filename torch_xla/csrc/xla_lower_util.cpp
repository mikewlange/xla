#include "torch_xla/csrc/xla_lower_util.h"

#include <algorithm>
#include <vector>

#include "tensorflow/compiler/xla/client/lib/arithmetic.h"
#include "tensorflow/compiler/xla/client/lib/comparators.h"
#include "tensorflow/compiler/xla/client/lib/constants.h"
#include "tensorflow/compiler/xla/client/lib/math.h"
#include "tensorflow/compiler/xla/client/lib/slicing.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/xla_client/debug_macros.h"
#include "tensorflow/compiler/xla/xla_client/util.h"
#include "torch_xla/csrc/convert_ops.h"
#include "torch_xla/csrc/data_ops.h"
#include "torch_xla/csrc/helpers.h"
#include "torch_xla/csrc/tensor_util.h"

namespace torch_xla {
namespace {

struct ConditionMaskData {
  xla::Shape iota_shape;
  xla::int64 flattened_size;
  xla::XlaOp reshaped_condition_int;
  xla::XlaOp length;
};

ConditionMaskData CreateConditionMaskData(const xla::XlaOp& condition) {
  xla::Shape iota_shape = XlaHelpers::ShapeOfXlaOp(condition);
  iota_shape.set_element_type(xla::PrimitiveType::S32);

  xla::int64 flattened_size = xla::Product(iota_shape.dimensions());
  xla::XlaOp reshaped_condition = xla::Reshape(condition, {flattened_size});
  xla::XlaOp zeros = xla::ZerosLike(reshaped_condition);
  xla::XlaOp zeros_int =
      xla::ConvertElementType(zeros, xla::PrimitiveType::S32);
  xla::XlaOp reshaped_condition_int =
      xla::ConvertElementType(reshaped_condition, xla::PrimitiveType::S32);
  xla::XlaOp compared = xla::ConvertElementType(
      xla::Gt(reshaped_condition_int, zeros_int), xla::PrimitiveType::S32);
  xla::XlaOp length = xla::ReduceAll(
      compared, xla::Zero(condition.builder(), xla::PrimitiveType::S32),
      xla::CreateScalarAddComputation(xla::PrimitiveType::S32,
                                      condition.builder()));
  return {std::move(iota_shape), flattened_size, reshaped_condition_int,
          length};
}

std::pair<xla::XlaOp, xla::Shape> DotExpand(const xla::XlaOp& op,
                                            const xla::Shape& op_shape,
                                            const xla::Shape& to_shape) {
  xla::int64 rank_delta = to_shape.rank() - op_shape.rank();
  XLA_CHECK_GT(rank_delta, 0) << op_shape << " vs. " << to_shape;

  std::vector<xla::int64> reshape_sizes(to_shape.rank(), 1);
  std::copy(op_shape.dimensions().begin(), op_shape.dimensions().end(),
            reshape_sizes.begin() + rank_delta);
  xla::XlaOp result = xla::Reshape(op, reshape_sizes);

  std::vector<xla::int64> broadcasted_sizes(
      to_shape.dimensions().begin(),
      to_shape.dimensions().begin() + rank_delta);
  broadcasted_sizes.insert(broadcasted_sizes.end(),
                           op_shape.dimensions().begin(),
                           op_shape.dimensions().end());
  return std::make_pair(
      xla::BroadcastInDim(result, broadcasted_sizes,
                          xla::util::Iota<xla::int64>(to_shape.rank())),
      xla::ShapeUtil::MakeShape(op_shape.element_type(), broadcasted_sizes));
}

std::pair<xla::XlaOp, xla::XlaOp> DotBroadcast(const xla::XlaOp& lhs,
                                               const xla::Shape& lhs_shape,
                                               const xla::XlaOp& rhs,
                                               const xla::Shape& rhs_shape) {
  auto lhs_dimensions = xla::util::ToVector<xla::int64>(lhs_shape.dimensions());
  auto rhs_dimensions = xla::util::ToVector<xla::int64>(rhs_shape.dimensions());
  XLA_CHECK_EQ(lhs_dimensions.size(), rhs_dimensions.size());
  for (xla::int64 i = 0; i < lhs_dimensions.size() - 2; ++i) {
    if (lhs_dimensions[i] == rhs_dimensions[i]) {
      continue;
    }
    if (lhs_dimensions[i] == 1) {
      lhs_dimensions[i] = rhs_dimensions[i];
    } else if (rhs_dimensions[i] == 1) {
      rhs_dimensions[i] = lhs_dimensions[i];
    } else {
      XLA_ERROR() << "Unsupported DotBroadcast: " << lhs_shape << " vs. "
                  << rhs_shape;
    }
  }

  xla::XlaOp broadcasted_lhs = lhs;
  xla::XlaOp broadcasted_rhs = rhs;
  if (lhs_dimensions != lhs_shape.dimensions()) {
    broadcasted_lhs =
        xla::BroadcastInDim(lhs, lhs_dimensions,
                            xla::util::Iota<xla::int64>(lhs_dimensions.size()));
  }
  if (rhs_dimensions != rhs_shape.dimensions()) {
    broadcasted_rhs =
        xla::BroadcastInDim(rhs, rhs_dimensions,
                            xla::util::Iota<xla::int64>(rhs_dimensions.size()));
  }
  return std::make_pair(broadcasted_lhs, broadcasted_rhs);
}

// Builds the computation for the given combiner.
xla::XlaComputation MakeScatterComputation(
    const std::function<xla::XlaOp(xla::XlaOp, xla::XlaOp)>& combiner,
    xla::PrimitiveType element_type) {
  xla::XlaBuilder cb("ScatterCombiner");
  xla::Shape xla_scalar_shape = xla::ShapeUtil::MakeShape(element_type, {});
  xla::XlaOp p0 = xla::Parameter(&cb, 0, xla_scalar_shape, "p0");
  xla::XlaOp result = xla::Parameter(&cb, 1, xla_scalar_shape, "p1");
  if (combiner != nullptr) {
    result = combiner(p0, result);
  }
  return ConsumeValue(cb.Build(result));
}

xla::XlaOp CreateIndexAlongDim(
    const xla::XlaOp& buffer, xla::int64 dim, const xla::XlaOp& index,
    const xla::XlaOp& value, bool broadcast_value_to_index,
    const std::function<xla::XlaOp(const xla::XlaOp&, const xla::XlaOp&)>&
        combiner) {
  const xla::Shape& buffer_shape = XlaHelpers::ShapeOfXlaOp(buffer);
  xla::ScatterDimensionNumbers dim_numbers;
  dim_numbers.set_index_vector_dim(1);
  for (xla::int64 window_dim = 0; window_dim < buffer_shape.rank();
       ++window_dim) {
    if (window_dim != dim) {
      dim_numbers.add_update_window_dims(window_dim);
    } else {
      dim_numbers.add_inserted_window_dims(window_dim);
      dim_numbers.add_scatter_dims_to_operand_dims(window_dim);
    }
  }

  // Broadcast the value to the right shape required by scatter.
  const xla::Shape& value_shape = XlaHelpers::ShapeOfXlaOp(value);
  xla::XlaOp updates = value;
  if (buffer_shape.element_type() != value_shape.element_type()) {
    updates = ConvertTo(updates, value_shape.element_type(),
                        buffer_shape.element_type(), /*device=*/nullptr);
  }
  if (broadcast_value_to_index) {
    const xla::Shape& index_shape = XlaHelpers::ShapeOfXlaOp(index);
    std::vector<xla::int64> update_dimensions =
        xla::util::ToVector<xla::int64>(buffer_shape.dimensions());
    update_dimensions[dim] = index_shape.dimensions(0);
    updates = xla::Broadcast(updates, update_dimensions);
  }
  // Create a combiner computation for the scatter.
  xla::XlaComputation combiner_computation =
      MakeScatterComputation(combiner, buffer_shape.element_type());
  return xla::Scatter(buffer, index, updates, combiner_computation,
                      dim_numbers);
}

bool ScatterRequiresPadding(const xla::Shape& input_shape,
                            const xla::Shape& index_shape) {
  bool requires_padding = false;
  for (size_t i = 0; i < input_shape.rank(); ++i) {
    if (input_shape.dimensions(i) > index_shape.dimensions(i)) {
      requires_padding = true;
    } else {
      XLA_CHECK_EQ(input_shape.dimensions(i), index_shape.dimensions(i));
    }
  }
  return requires_padding;
}

xla::XlaOp XlaDenseScatter(
    const xla::XlaOp& input, const xla::XlaOp& index, const xla::XlaOp& src,
    xla::int64 dim,
    const std::function<xla::XlaOp(const xla::XlaOp&, const xla::XlaOp&)>&
        combiner) {
  // Contribute back this code to xla::TorchScatterDense() once this has reached
  // a stable implementation.
  xla::XlaBuilder* builder = input.builder();
  return builder->ReportErrorOrReturn([&]() -> xla::StatusOr<xla::XlaOp> {
    TF_ASSIGN_OR_RETURN(xla::Shape index_shape, builder->GetShape(index));
    TF_ASSIGN_OR_RETURN(xla::Shape input_shape, builder->GetShape(input));
    std::vector<xla::int64> index_broacast_dims;
    std::vector<xla::int64> sizes;
    for (xla::int64 i = 0; i < index_shape.rank(); ++i) {
      if (i < dim) {
        index_broacast_dims.push_back(i);
      } else {
        if (i == dim) {
          sizes.push_back(input_shape.dimensions(i));
        }
        index_broacast_dims.push_back(i + 1);
      }
      sizes.push_back(index_shape.dimensions(i));
    }

    xla::XlaOp mask = xla::Eq(
        xla::BroadcastInDim(index, sizes, index_broacast_dims),
        xla::Iota(builder,
                  xla::ShapeUtil::MakeShape(index_shape.element_type(), sizes),
                  dim));
    xla::XlaOp selected_src = xla::Select(
        mask, xla::BroadcastInDim(src, sizes, index_broacast_dims),
        xla::Zeros(builder, xla::ShapeUtil::MakeShape(
                                input_shape.element_type(), sizes)));
    xla::XlaOp masked_src = xla::Reduce(
        selected_src, xla::Zero(builder, input_shape.element_type()),
        xla::CreateScalarIdentityWithZeroComputation(input_shape.element_type(),
                                                     builder),
        {dim + 1});
    if (index_shape.dimensions() == input_shape.dimensions()) {
      // If the index shape is the same as the input shape, the input shape will
      // be fully covered (since scatter indices must be unique), so there is no
      // need for masking.
      return combiner != nullptr ? combiner(input, masked_src) : masked_src;
    }
    xla::XlaOp reduced_mask = xla::Reduce(
        mask, xla::ConstantR0<bool>(builder, false),
        xla::CreateScalarOrComputation(xla::PrimitiveType::PRED, builder),
        {dim + 1});
    if (ScatterRequiresPadding(input_shape, index_shape)) {
      masked_src =
          PadToSize(masked_src, xla::Zero(builder, input_shape.element_type()),
                    input_shape.dimensions());
      reduced_mask =
          PadToSize(reduced_mask, xla::ConstantR0<bool>(builder, false),
                    input_shape.dimensions());
    }
    xla::XlaOp result;
    if (combiner != nullptr) {
      result = xla::Select(reduced_mask, combiner(input, masked_src), input);
    } else {
      result = xla::Select(reduced_mask, masked_src, input);
    }
    return result;
  });
}

std::vector<xla::XlaOp> BuildConditionIndices(const xla::XlaOp& condition) {
  ConditionMaskData cmd = CreateConditionMaskData(condition);
  std::vector<xla::XlaOp> to_sort = {cmd.reshaped_condition_int};
  std::vector<xla::PrimitiveType> types_to_sort = {xla::PrimitiveType::S32};
  for (xla::int64 axis = 0; axis < cmd.iota_shape.rank(); ++axis) {
    xla::XlaOp iota = xla::Iota(condition.builder(), cmd.iota_shape, axis);
    xla::XlaOp reshaped = xla::Reshape(iota, {cmd.flattened_size});
    to_sort.push_back(reshaped);
    types_to_sort.push_back(xla::PrimitiveType::S32);
  }

  xla::XlaOp sorted = xla::Sort(
      to_sort,
      xla::CreateScalarGtComputation(types_to_sort, condition.builder()),
      /*dimension=*/0,
      /*is_stable=*/true);
  std::vector<xla::XlaOp> to_concat;
  for (xla::int64 i = 0; i < cmd.iota_shape.rank(); ++i) {
    xla::XlaOp index_single_dim = xla::GetTupleElement(sorted, i + 1);
    to_concat.push_back(
        xla::Reshape(index_single_dim, {cmd.flattened_size, 1}));
  }

  xla::XlaOp result = xla::ConcatInDim(condition.builder(), to_concat, 1);
  xla::XlaOp result_padded = xla::SetDimensionSize(result, cmd.length, 0);
  return {result_padded, cmd.length};
}

}  // namespace

xla::XlaOp PadToSize(const xla::XlaOp& input, const xla::XlaOp& pad_value,
                     tensorflow::gtl::ArraySlice<const xla::int64> size) {
  const xla::Shape& input_shape = XlaHelpers::ShapeOfXlaOp(input);
  XLA_CHECK_EQ(input_shape.rank(), size.size());

  xla::PaddingConfig padding_config;
  for (size_t i = 0; i < size.size(); i++) {
    auto* dims = padding_config.add_dimensions();
    dims->set_edge_padding_low(0);
    dims->set_interior_padding(0);
    XLA_CHECK_GE(size[i], input_shape.dimensions(i));
    dims->set_edge_padding_high(size[i] - input_shape.dimensions(i));
  }
  return xla::Pad(input, pad_value, padding_config);
}

std::vector<xla::XlaOp> CreateKthValue(const xla::XlaOp& input, xla::int64 k,
                                       xla::int64 dim, bool keepdim) {
  // Here 'k' is 1 based (1...).
  const xla::Shape& shape = XlaHelpers::ShapeOfXlaOp(input);
  XLA_CHECK_LE(k, shape.dimensions(dim));
  xla::Shape iota_shape =
      xla::ShapeUtil::MakeShape(xla::PrimitiveType::S32, shape.dimensions());
  xla::XlaOp iota = xla::Iota(input.builder(), iota_shape, dim);
  xla::XlaOp sort_result = xla::Sort(
      {input, iota},
      xla::CreateScalarLtComputation(
          {shape.element_type(), xla::PrimitiveType::S32}, input.builder()),
      dim);

  std::vector<xla::int64> start_indices(shape.rank(), 0);
  start_indices[dim] = k - 1;
  std::vector<xla::int64> limit_indices(shape.dimensions().begin(),
                                        shape.dimensions().end());
  limit_indices[dim] = k;
  std::vector<xla::int64> strides(shape.rank(), 1);

  xla::XlaOp values = xla::Slice(xla::GetTupleElement(sort_result, 0),
                                 start_indices, limit_indices, strides);
  xla::XlaOp indices = xla::Slice(xla::GetTupleElement(sort_result, 1),
                                  start_indices, limit_indices, strides);
  if (!keepdim) {
    auto reshape_sizes = XlaHelpers::DropDimensions(shape.dimensions(), {dim});
    values = xla::Reshape(values, reshape_sizes);
    indices = xla::Reshape(indices, reshape_sizes);
  }
  // aten::kthvalue() wants Long tensors as indices.
  return {values, xla::ConvertElementType(
                      indices, GetDevicePrimitiveType(xla::PrimitiveType::S64,
                                                      /*device=*/nullptr))};
}

std::vector<xla::XlaOp> CreateTopK(const xla::XlaOp& input, xla::int64 k,
                                   xla::int64 dim, bool largest,
                                   bool /* sorted */) {
  // Here 'k' is 1 based (1...).
  const xla::Shape& shape = XlaHelpers::ShapeOfXlaOp(input);
  XLA_CHECK_LE(k, shape.dimensions(dim));
  xla::Shape iota_shape =
      xla::ShapeUtil::MakeShape(xla::PrimitiveType::S32, shape.dimensions());
  xla::XlaOp iota = xla::Iota(input.builder(), iota_shape, dim);
  xla::XlaComputation comparator =
      largest ? xla::CreateScalarGtComputation(
                    {shape.element_type(), xla::PrimitiveType::S32},
                    input.builder())
              : xla::CreateScalarLtComputation(
                    {shape.element_type(), xla::PrimitiveType::S32},
                    input.builder());
  xla::XlaOp sort_result = xla::Sort({input, iota}, comparator, dim);

  std::vector<xla::int64> start_indices(shape.rank(), 0);
  std::vector<xla::int64> limit_indices(shape.dimensions().begin(),
                                        shape.dimensions().end());
  limit_indices[dim] = k;
  std::vector<xla::int64> strides(shape.rank(), 1);

  xla::XlaOp values = xla::Slice(xla::GetTupleElement(sort_result, 0),
                                 start_indices, limit_indices, strides);
  xla::XlaOp indices = xla::Slice(xla::GetTupleElement(sort_result, 1),
                                  start_indices, limit_indices, strides);
  // aten::topk() wants Long tensors as indices.
  return {values, xla::ConvertElementType(
                      indices, GetDevicePrimitiveType(xla::PrimitiveType::S64,
                                                      /*device=*/nullptr))};
}

xla::XlaOp CreateMatMul(const xla::XlaOp& lhs, const xla::XlaOp& rhs) {
  const auto precision_level = XlaHelpers::mat_mul_precision();
  xla::PrecisionConfig precision_config =
      XlaHelpers::BuildPrecisionConfig(precision_level);
  // Expand cases in https://pytorch.org/docs/stable/torch.html#torch.matmul
  xla::Shape lhs_shape = XlaHelpers::ShapeOfXlaOp(lhs);
  xla::Shape rhs_shape = XlaHelpers::ShapeOfXlaOp(rhs);
  if ((lhs_shape.rank() == 1 && rhs_shape.rank() == 1) ||
      (lhs_shape.rank() == 2 && rhs_shape.rank() == 2) ||
      (lhs_shape.rank() == 2 && rhs_shape.rank() == 1)) {
    return xla::Dot(lhs, rhs);
  }
  if (lhs_shape.rank() == 1 && rhs_shape.rank() == 2) {
    xla::XlaOp reshaped_lhs = xla::Reshape(lhs, {1, lhs_shape.dimensions(0)});
    return xla::Reshape(xla::Dot(reshaped_lhs, rhs), {rhs_shape.dimensions(1)});
  }
  if (lhs_shape.rank() >= 1 && rhs_shape.rank() >= 1 &&
      (lhs_shape.rank() >= 3 || rhs_shape.rank() >= 3)) {
    xla::XlaOp reshaped_lhs = lhs;
    xla::XlaOp reshaped_rhs = rhs;
    if (lhs_shape.rank() > rhs_shape.rank()) {
      std::tie(reshaped_rhs, rhs_shape) =
          DotExpand(reshaped_rhs, rhs_shape, lhs_shape);
    } else if (rhs_shape.rank() > lhs_shape.rank()) {
      std::tie(reshaped_lhs, lhs_shape) =
          DotExpand(reshaped_lhs, lhs_shape, rhs_shape);
    }
    std::tie(reshaped_lhs, reshaped_rhs) =
        DotBroadcast(reshaped_lhs, lhs_shape, reshaped_rhs, rhs_shape);

    // At this point lhs and rhs ranks are the same, use left rank in code
    // below.
    xla::DotDimensionNumbers dims;
    for (xla::int64 i = 0; i < lhs_shape.rank() - 2; ++i) {
      dims.add_lhs_batch_dimensions(i);
      dims.add_rhs_batch_dimensions(i);
    }
    dims.add_lhs_contracting_dimensions(lhs_shape.rank() - 1);
    dims.add_rhs_contracting_dimensions(lhs_shape.rank() - 2);

    return xla::DotGeneral(reshaped_lhs, reshaped_rhs, dims, &precision_config);
  }
  XLA_ERROR() << "Unsupported matmul operation: matmul(" << lhs_shape << ", "
              << rhs_shape << ")";
}

xla::XlaOp BuildBernoulli(const xla::XlaOp& probability,
                          const xla::Shape& shape) {
  const xla::Shape& probability_shape = XlaHelpers::ShapeOfXlaOp(probability);
  xla::XlaOp zero = XlaHelpers::ScalarValue<float>(
      0, probability_shape.element_type(), probability.builder());
  xla::XlaOp one = XlaHelpers::ScalarValue<float>(
      1, probability_shape.element_type(), probability.builder());
  xla::XlaOp noise = xla::RngUniform(zero, one, probability_shape);
  return xla::ConvertElementType(xla::Lt(noise, probability),
                                 shape.element_type());
}

xla::XlaOp BuildDropout(const xla::XlaOp& input, float probability) {
  const xla::Shape& shape = XlaHelpers::ShapeOfXlaOp(input);
  xla::XlaOp prob =
      XlaHelpers::ScalarBroadcast<float>(probability, shape, input.builder());
  xla::XlaOp mask = BuildBernoulli(prob, shape);
  if (probability > 0.0f) {
    mask = mask / prob;
  }
  return input * mask;
}

xla::XlaOp BuildRandperm(xla::int64 n, xla::PrimitiveType element_type,
                         xla::XlaBuilder* builder) {
  xla::XlaOp input = xla::Iota(builder, element_type, n);
  // Ensure that the key space is greater than or equal to the cube of the
  // number of values to manage the number of collisions. Inspired by
  // RandomShuffleOp in tf2xla, where the full rationale for picking the
  // exponent value is described.
  const int kExponent = 3;
  const int rounds = static_cast<int>(
      std::ceil(kExponent * std::log(n) / std::log(tensorflow::kuint32max)));
  const xla::Shape key_shape = xla::ShapeUtil::MakeShape(xla::U32, {n});
  xla::XlaOp zero = xla::ConstantR0(builder, 0U);
  xla::XlaOp max_value = xla::ConstantR0(builder, tensorflow::kuint32max);

  xla::XlaOp curr = input;
  for (int i = 0; i < rounds; ++i) {
    xla::XlaOp keys = xla::RngUniform(zero, max_value, key_shape);
    xla::XlaOp sorted = xla::Sort(
        {keys, curr},
        xla::CreateScalarLtComputation({xla::U32, element_type}, builder));
    curr = xla::GetTupleElement(sorted, 1);
  }
  return curr;
}

std::vector<xla::XlaOp> CreateBroadcastTensors(
    tensorflow::gtl::ArraySlice<const xla::XlaOp> operands) {
  xla::Shape result_shape = XlaHelpers::ShapeOfXlaOp(operands.front());
  std::vector<xla::Shape> operand_shapes;
  for (const xla::XlaOp operand : operands) {
    const xla::Shape& operand_shape = XlaHelpers::ShapeOfXlaOp(operand);
    operand_shapes.push_back(operand_shape);
    result_shape = XlaHelpers::GetPromotedShape(result_shape, operand_shape);
  }
  std::vector<xla::XlaOp> result;
  for (size_t i = 0; i < operands.size(); ++i) {
    result.push_back(XlaHelpers::ImplicitBroadcast(
        operands[i], operand_shapes[i], result_shape));
  }
  return result;
}

xla::XlaOp CreateIndex(const xla::XlaOp& input, const xla::XlaOp& indices,
                       xla::int64 start_dim) {
  const xla::Shape& input_shape = XlaHelpers::ShapeOfXlaOp(input);
  const xla::Shape& indices_shape = XlaHelpers::ShapeOfXlaOp(indices);
  XLA_CHECK_GE(indices_shape.rank(), 1);
  xla::int64 num_index_dims =
      indices_shape.dimensions(indices_shape.rank() - 1);
  xla::GatherDimensionNumbers dim_numbers;
  std::vector<xla::int64> slice_sizes;
  slice_sizes.reserve(input_shape.rank());
  for (xla::int64 i = 0; i < input_shape.rank(); ++i) {
    if (i >= start_dim && i < num_index_dims + start_dim) {
      dim_numbers.add_collapsed_slice_dims(i);
      slice_sizes.push_back(1);
    } else {
      slice_sizes.push_back(input_shape.dimensions(i));
      xla::int64 indices_rank = indices_shape.rank() - 1;
      if (i < start_dim) {
        dim_numbers.add_offset_dims(i);
      } else {
        dim_numbers.add_offset_dims(i - num_index_dims + indices_rank);
      }
    }
  }
  dim_numbers.set_index_vector_dim(indices_shape.rank() - 1);
  for (xla::int64 i = 0; i < num_index_dims; i++) {
    dim_numbers.add_start_index_map(i + start_dim);
  }
  return xla::Gather(input, indices, dim_numbers, slice_sizes);
}

xla::XlaOp CreateIndexUpdate(
    const xla::XlaOp& buffer, const xla::XlaOp& indices, xla::int64 start_dim,
    const xla::XlaOp& values,
    const std::function<xla::XlaOp(const xla::XlaOp&, const xla::XlaOp&)>&
        combiner) {
  const xla::Shape& buffer_shape = XlaHelpers::ShapeOfXlaOp(buffer);
  const xla::Shape& indices_shape = XlaHelpers::ShapeOfXlaOp(indices);
  const xla::Shape& values_shape = XlaHelpers::ShapeOfXlaOp(values);

  absl::Span<const xla::int64> indices_dims =
      xla::AsInt64Slice(indices_shape.dimensions());
  XLA_CHECK(!indices_dims.empty());
  // The minor dimension of indices contains the indices to update.
  xla::int64 num_index_dims = indices_dims.back();
  indices_dims.remove_suffix(1);
  xla::ScatterDimensionNumbers dim_numbers;
  dim_numbers.set_index_vector_dim(indices_shape.rank() - 1);

  xla::int64 values_rank = values_shape.rank();
  xla::int64 buffer_rank = buffer_shape.rank();
  xla::int64 num_window_dims_in_values = buffer_rank - num_index_dims;

  // Make the values match the rank expected by scatter.
  std::vector<xla::int64> expected_values_dims;
  for (xla::int64 dim = 0; dim < start_dim; ++dim) {
    expected_values_dims.push_back(buffer_shape.dimensions(dim));
  }
  expected_values_dims.insert(expected_values_dims.end(), indices_dims.begin(),
                              indices_dims.end());
  for (xla::int64 dim = num_index_dims + start_dim; dim < buffer_rank; ++dim) {
    expected_values_dims.push_back(buffer_shape.dimensions(dim));
  }
  xla::XlaOp new_values = values;
  if (buffer_shape.element_type() != values_shape.element_type()) {
    new_values = ConvertTo(new_values, values_shape.element_type(),
                           buffer_shape.element_type(), /*device=*/nullptr);
  }
  new_values = BuildExpand(new_values, expected_values_dims);
  const xla::Shape& new_values_shape = XlaHelpers::ShapeOfXlaOp(new_values);
  values_rank = new_values_shape.rank();

  for (xla::int64 dim = 0; dim < start_dim; ++dim) {
    dim_numbers.add_update_window_dims(dim);
  }
  for (xla::int64 i = values_rank - num_window_dims_in_values + start_dim;
       i < values_rank; ++i) {
    dim_numbers.add_update_window_dims(i);
  }
  for (xla::int64 i = 0; i < num_index_dims; ++i) {
    dim_numbers.add_inserted_window_dims(i + start_dim);
    dim_numbers.add_scatter_dims_to_operand_dims(i + start_dim);
  }
  xla::XlaComputation combiner_computation =
      MakeScatterComputation(combiner, buffer_shape.element_type());
  return xla::Scatter(buffer, indices, new_values, combiner_computation,
                      dim_numbers);
}

xla::XlaOp CreateIndexAdd(const xla::XlaOp& buffer, xla::int64 dim,
                          const xla::XlaOp& index, const xla::XlaOp& value) {
  auto add_scatter_combiner = [](const xla::XlaOp& x,
                                 const xla::XlaOp& y) -> xla::XlaOp {
    return x + y;
  };
  return CreateIndexAlongDim(buffer, dim, index, value,
                             /*broadcast_value_to_index=*/false,
                             add_scatter_combiner);
}

xla::XlaOp CreateIndexCopy(const xla::XlaOp& buffer, xla::int64 dim,
                           const xla::XlaOp& index, const xla::XlaOp& value) {
  return CreateIndexAlongDim(buffer, dim, index, value,
                             /*broadcast_value_to_index=*/false, nullptr);
}

xla::XlaOp CreateIndexFill(const xla::XlaOp& buffer, xla::int64 dim,
                           const xla::XlaOp& index, const xla::XlaOp& value) {
  return CreateIndexAlongDim(buffer, dim, index, value,
                             /*broadcast_value_to_index=*/true, nullptr);
}

XlaOpCombiner NumericAddCombiner() {
  return [](const xla::XlaOp& x, const xla::XlaOp& y) -> xla::XlaOp {
    xla::XlaOp numeric_x = ConvertToNumeric(x);
    xla::XlaOp numeric_y = ConvertToNumeric(y);
    xla::XlaOp numeric_sum = numeric_x + numeric_y;
    return ConvertTo(numeric_sum, XlaHelpers::TypeOfXlaOp(numeric_sum),
                     XlaHelpers::TypeOfXlaOp(x),
                     /*device=*/nullptr);
  };
}

xla::XlaOp CreateScatter(const xla::XlaOp& input, const xla::XlaOp& index,
                         const xla::XlaOp& source, xla::int64 dim,
                         const XlaOpCombiner& combiner) {
  static int dense_scatter_factor =
      xla::sys_util::GetEnvInt("XLA_DENSE_SCATTER_FACTOR", 100);
  const xla::Shape& input_shape = XlaHelpers::ShapeOfXlaOp(input);
  xla::Shape index_shape = XlaHelpers::ShapeOfXlaOp(index);
  const xla::Shape& source_shape = XlaHelpers::ShapeOfXlaOp(source);
  XLA_CHECK_EQ(source_shape.rank(), index_shape.rank());
  xla::XlaOp source_op = source;
  if (source_shape.dimensions() != index_shape.dimensions()) {
    std::vector<xla::int64> base_indices(source_shape.rank(), 0);
    source_op = BuildSlice(source_op, base_indices, index_shape.dimensions());
  }

  xla::int64 input_elements = xla::ShapeUtil::ElementsIn(input_shape);
  xla::int64 index_elements = xla::ShapeUtil::ElementsIn(index_shape);
  if (index_elements >= input_elements / dense_scatter_factor) {
    return XlaDenseScatter(input, index, source_op, dim, combiner);
  }

  xla::ShapeUtil::AppendMajorDimension(1, &index_shape);
  std::vector<xla::XlaOp> to_concat;
  to_concat.reserve(input_shape.rank());
  for (xla::int64 i = 0; i < input_shape.rank(); ++i) {
    if (i == dim) {
      to_concat.push_back(xla::Reshape(index, index_shape.dimensions()));
    } else {
      to_concat.push_back(xla::Iota(input.builder(), index_shape, i));
    }
  }
  xla::XlaOp scatter_indices =
      xla::ConcatInDim(input.builder(), to_concat, input_shape.rank());
  xla::ScatterDimensionNumbers scatter_dnums;
  scatter_dnums.set_index_vector_dim(input_shape.rank());
  for (xla::int64 i = 0; i < input_shape.rank(); ++i) {
    scatter_dnums.add_inserted_window_dims(i);
    scatter_dnums.add_scatter_dims_to_operand_dims(i);
  }
  return xla::Scatter(
      input, scatter_indices, source_op,
      MakeScatterComputation(combiner, input_shape.element_type()),
      scatter_dnums);
}

xla::XlaOp CreatePut(const xla::XlaOp& input, const xla::XlaOp& index,
                     const xla::XlaOp& source, bool accumulate) {
  xla::Shape input_shape;
  xla::XlaOp r1_input = XlaHelpers::Flatten(input, &input_shape);
  xla::Shape index_shape;
  xla::XlaOp r1_index = XlaHelpers::Flatten(index, &index_shape);
  xla::XlaOp max_index =
      XlaHelpers::ScalarValue(xla::ShapeUtil::ElementsIn(input_shape),
                              index_shape.element_type(), index.builder());
  xla::XlaOp bound_index = BoundIndices(r1_index, max_index);
  xla::XlaOp r1_source = XlaHelpers::Flatten(source);
  XlaOpCombiner combiner;
  if (accumulate) {
    combiner = NumericAddCombiner();
  }
  xla::XlaOp r1_scatter =
      CreateScatter(r1_input, bound_index, r1_source, /*dim=*/0, combiner);
  return xla::Reshape(r1_scatter, input_shape.dimensions());
}

std::vector<xla::XlaOp> BuildNonZero(const xla::XlaOp& input) {
  const xla::Shape& input_shape = XlaHelpers::ShapeOfXlaOp(input);
  return BuildConditionIndices(
      xla::Ne(input, xla::Zero(input.builder(), input_shape.element_type())));
}

std::vector<xla::XlaOp> BuildMaskedSelect(const xla::XlaOp& input,
                                          const xla::XlaOp& mask) {
  xla::Shape input_shape;
  xla::XlaOp r1_input = XlaHelpers::Flatten(input, &input_shape);
  const xla::Shape& mask_shape = XlaHelpers::ShapeOfXlaOp(mask);
  xla::Shape promoted_mask_shape =
      XlaHelpers::GetPromotedShape(mask_shape, input_shape);
  xla::XlaOp bcast_mask =
      XlaHelpers::ImplicitBroadcast(mask, mask_shape, promoted_mask_shape);
  xla::XlaOp r1_bcast_mask = XlaHelpers::Flatten(bcast_mask);

  ConditionMaskData cmd = CreateConditionMaskData(r1_bcast_mask);
  std::vector<xla::XlaOp> to_sort = {cmd.reshaped_condition_int, r1_input};
  std::vector<xla::PrimitiveType> types_to_sort = {xla::PrimitiveType::S32,
                                                   input_shape.element_type()};
  xla::XlaOp sorted = xla::Sort(
      to_sort, xla::CreateScalarGtComputation(types_to_sort, input.builder()),
      /*dimension=*/0,
      /*is_stable=*/true);
  xla::XlaOp sorted_input = xla::GetTupleElement(sorted, 1);
  xla::XlaOp sorted_input_padded =
      xla::SetDimensionSize(sorted_input, cmd.length, 0);
  return {sorted_input_padded, cmd.length};
}

}  // namespace torch_xla
