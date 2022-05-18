/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fn
 */

#include "FN_lazy_function_execute_eager.hh"

namespace blender::fn {

BasicLazyFunctionParams::BasicLazyFunctionParams(
    const LazyFunction &fn,
    void *storage,
    const Span<GMutablePointer> inputs,
    const Span<GMutablePointer> outputs,
    MutableSpan<std::optional<ValueUsage>> input_usages,
    Span<ValueUsage> output_usages,
    MutableSpan<bool> set_outputs)
    : LazyFunctionParams(fn, storage),
      inputs_(inputs),
      outputs_(outputs),
      input_usages_(input_usages),
      output_usages_(output_usages),
      set_outputs_(set_outputs)
{
}

void *BasicLazyFunctionParams::try_get_input_data_ptr_impl(const int index) const
{
  return inputs_[index].get();
}

void *BasicLazyFunctionParams::try_get_input_data_ptr_or_request_impl(const int index)
{
  void *value = inputs_[index].get();
  if (value == nullptr) {
    input_usages_[index] = ValueUsage::Used;
  }
  return value;
}

void *BasicLazyFunctionParams::get_output_data_ptr_impl(const int index)
{
  return outputs_[index].get();
}

void BasicLazyFunctionParams::output_set_impl(const int index)
{
  set_outputs_[index] = true;
}

bool BasicLazyFunctionParams::output_was_set_impl(const int index) const
{
  return set_outputs_[index];
}

ValueUsage BasicLazyFunctionParams::get_output_usage_impl(const int index) const
{
  return output_usages_[index];
}

void BasicLazyFunctionParams::set_input_unused_impl(const int index)
{
  input_usages_[index] = ValueUsage::Unused;
}

}  // namespace blender::fn
