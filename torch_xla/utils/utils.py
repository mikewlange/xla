from __future__ import division
from __future__ import print_function

import copy
import os
import shutil
import sys
import tempfile
import time


class Cleaner(object):

  def __init__(self, func):
    self.func = func

  def __del__(self):
    self.func()


class TmpFolder(object):

  def __init__(self):
    self.name = tempfile.mkdtemp()
    self.cleaner = Cleaner(lambda: shutil.rmtree(self.name))


class SampleGenerator(object):
  """Iterator which returns multiple samples of a given input data.

  Can be used in place of a PyTorch `DataLoader` to generate synthetic data.

  Args:
    data: The data which should be returned at each iterator step.
    sample_count: The maximum number of `data` samples to be returned.
  """

  def __init__(self, data, sample_count):
    self._data = data
    self._sample_count = sample_count
    self._count = 0

  def __iter__(self):
    return SampleGenerator(self._data, self._sample_count)

  def __len__(self):
    return self._sample_count

  def __next__(self):
    return self.next()

  def next(self):
    if self._count >= self._sample_count:
      raise StopIteration
    self._count += 1
    return self._data


class FnDataGenerator(object):

  def __init__(self, func, batch_size, gen_tensor, dims=None, count=1):
    self._func = func
    self._batch_size = batch_size
    self._gen_tensor = gen_tensor
    self._dims = list(dims) if dims else [1]
    self._count = count
    self._emitted = 0

  def __len__(self):
    return self._count

  def __iter__(self):
    return FnDataGenerator(
        self._func,
        self._batch_size,
        self._gen_tensor,
        dims=self._dims,
        count=self._count)

  def __next__(self):
    return self.next()

  def next(self):
    if self._emitted >= self._count:
      raise StopIteration
    data = self._gen_tensor(self._batch_size, *self._dims)
    target = self._func(data)
    self._emitted += 1
    return data, target


def as_list(t):
  return t if isinstance(t, (tuple, list)) else [t]


def getenv_as(name, type, defval=None):
  env = os.environ.get(name, None)
  if type == bool:
    return defval if env is None else type(int(env))
  return defval if env is None else type(env)


def for_each_instance(value, select_fn, fn):
  if select_fn(value):
    fn(value)
  elif isinstance(value, dict):
    for k, v in value.items():
      for_each_instance(k, select_fn, fn)
      for_each_instance(v, select_fn, fn)
  elif isinstance(value, (list, tuple, set)):
    for x in value:
      for_each_instance(x, select_fn, fn)
  elif hasattr(value, '__dict__'):
    for k in value.__dict__.keys():
      for_each_instance(value.__dict__[k], select_fn, fn)


def for_each_instance_rewrite(value, select_fn, fn):
  if select_fn(value):
    return fn(value)
  elif isinstance(value, dict):
    result = dict()
    for k, v in value.items():
      k = for_each_instance_rewrite(k, select_fn, fn)
      result[k] = for_each_instance_rewrite(v, select_fn, fn)
    return result
  elif isinstance(value, (list, tuple, set)):
    result = []
    for x in value:
      result.append(for_each_instance_rewrite(x, select_fn, fn))
    return type(value)(result)
  elif hasattr(value, '__dict__'):
    result = copy.copy(value)
    for k in result.__dict__.keys():
      v = for_each_instance_rewrite(result.__dict__[k], select_fn, fn)
      result.__dict__[k] = v
    return result
  return value


def shape(inputs):
  cshape = []
  if isinstance(inputs, (list, tuple)):
    lshape = None
    for input in inputs:
      ishape = shape(input)
      if lshape is None:
        lshape = ishape
      else:
        assert lshape == ishape
    cshape.extend([len(inputs)] + (lshape or []))
  return cshape


def flatten_nested_tuple(inputs):
  flat = []
  if isinstance(inputs, (list, tuple)):
    for input in inputs:
      flat.extend(flatten_nested_tuple(input))
  else:
    flat.append(inputs)
  return tuple(flat)


def list_copy_append(ilist, item):
  ilist_copy = list(ilist)
  ilist_copy.append(item)
  return ilist_copy


def null_print(*args, **kwargs):
  return


def eprint(*args, **kwargs):
  print(*args, file=sys.stderr, **kwargs)


def get_print_fn(debug=None):
  if debug is None:
    debug = int(os.environ.get('DEBUG', '0'))
  return eprint if debug else null_print


def timed(fn, msg='', printfn=eprint):
  if printfn is None:
    printfn = get_print_fn()
  s = time.time()
  result = fn()
  printfn('{}{:.3f}ms'.format(msg, 1000.0 * (time.time() - s)))
  return result


class TimedScope(object):

  def __init__(self, msg='', printfn=eprint):
    if printfn is None:
      printfn = get_print_fn()
    self._msg = msg
    self._printfn = printfn
    self._error = None

  def __enter__(self):
    self._start = time.time()
    return self

  def __exit__(self, type, value, traceback):
    if self._error is None:
      self._printfn('{}{:.3f}ms'.format(self._msg,
                                        1000.0 * (time.time() - self._start)))

  def set_error(self, error):
    self._error = error
