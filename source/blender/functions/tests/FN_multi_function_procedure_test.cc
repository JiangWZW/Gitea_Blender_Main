/* Apache License, Version 2.0 */

#include "testing/testing.h"

#include "FN_multi_function_builder.hh"
#include "FN_multi_function_procedure_builder.hh"
#include "FN_multi_function_procedure_executor.hh"
#include "FN_multi_function_test_common.hh"

namespace blender::fn::tests {

TEST(multi_function_procedure, SimpleTest)
{
  /**
   * procedure(int var1, int var2, int *var4) {
   *   int var3 = var1 + var2;
   *   var4 = var2 + var3;
   * }
   */

  CustomMF_SI_SI_SO<int, int, int> add_fn{"add", [](int a, int b) { return a + b; }};
  CustomMF_SM<int> add_10_fn{"add_10", [](int &a) { a += 10; }};

  MFProcedure procedure;
  MFProcedureBuilder builder{procedure};

  MFVariable *var1 = &builder.add_single_input_parameter<int>();
  MFVariable *var2 = &builder.add_single_input_parameter<int>();
  auto [var3] = builder.add_call_with_new_variables<1>(add_fn, {var1, var2});
  auto [var4] = builder.add_call_with_new_variables<1>(add_fn, {var2, var3});
  builder.add_call(add_10_fn, {var4});
  builder.add_destruct({var1, var2, var3});
  builder.add_output_parameter(*var4);

  MFProcedureExecutor executor{"My Procedure", procedure};

  MFParamsBuilder params{executor, 3};
  MFContextBuilder context;

  Array<int> input_array = {1, 2, 3};
  params.add_readonly_single_input(input_array.as_span());
  params.add_readonly_single_input_value(3);

  Array<int> output_array(3);
  params.add_uninitialized_single_output(output_array.as_mutable_span());

  executor.call(IndexRange(3), params, context);

  EXPECT_EQ(output_array[0], 17);
  EXPECT_EQ(output_array[1], 18);
  EXPECT_EQ(output_array[2], 19);
}

TEST(multi_function_procedure, BranchTest)
{
  /**
   * procedure(int &var1, bool var2) {
   *   if (var2) {
   *     var1 += 100;
   *   }
   *   else {
   *     var1 += 10;
   *   }
   *   var1 += 10;
   * }
   */

  CustomMF_SM<int> add_10_fn{"add_10", [](int &a) { a += 10; }};
  CustomMF_SM<int> add_100_fn{"add_100", [](int &a) { a += 100; }};

  MFProcedure procedure;
  MFProcedureBuilder builder{procedure};

  MFVariable *var1 = &builder.add_single_mutable_parameter<int>();
  MFVariable *var2 = &builder.add_single_input_parameter<bool>();

  MFProcedureBuilder::Branch branch = builder.add_branch(*var2);
  branch.branch_false.add_call(add_10_fn, {var1});
  branch.branch_true.add_call(add_100_fn, {var1});
  builder.set_cursor_after_branch(branch);
  builder.add_call(add_10_fn, {var1});
  builder.add_destruct({var2});

  MFProcedureExecutor procedure_fn{"Condition Test", procedure};
  MFParamsBuilder params(procedure_fn, 5);

  Array<int> values_a = {1, 5, 3, 6, 2};
  Array<bool> values_cond = {true, false, true, true, false};

  params.add_single_mutable(values_a.as_mutable_span());
  params.add_readonly_single_input(values_cond.as_span());

  MFContextBuilder context;
  procedure_fn.call({1, 2, 3, 4}, params, context);

  EXPECT_EQ(values_a[0], 1);
  EXPECT_EQ(values_a[1], 25);
  EXPECT_EQ(values_a[2], 113);
  EXPECT_EQ(values_a[3], 116);
  EXPECT_EQ(values_a[4], 22);
}

TEST(multi_function_procedure, EvaluateOne)
{
  /**
   * procedure(int var1, int var2) {
   *   var2 = var1 + 10;
   * }
   */

  int tot_evaluations = 0;
  CustomMF_SI_SO<int, int> add_10_fn{"add_10", [&](int a) {
                                       tot_evaluations++;
                                       return a + 10;
                                     }};

  MFProcedure procedure;
  MFProcedureBuilder builder{procedure};

  MFVariable *var1 = &builder.add_single_input_parameter<int>();
  auto [var2] = builder.add_call_with_new_variables<1>(add_10_fn, {var1});
  builder.add_destruct(*var1);
  builder.add_output_parameter(*var2);

  MFProcedureExecutor procedure_fn{"Evaluate One", procedure};
  MFParamsBuilder params{procedure_fn, 5};

  Array<int> values_out = {1, 2, 3, 4, 5};
  params.add_readonly_single_input_value(1);
  params.add_uninitialized_single_output(values_out.as_mutable_span());

  MFContextBuilder context;
  procedure_fn.call({0, 1, 3, 4}, params, context);

  EXPECT_EQ(values_out[0], 11);
  EXPECT_EQ(values_out[1], 11);
  EXPECT_EQ(values_out[2], 3);
  EXPECT_EQ(values_out[3], 11);
  EXPECT_EQ(values_out[4], 11);
  /* We expect only one evaluation, because the input is constant. */
  EXPECT_EQ(tot_evaluations, 1);
}

TEST(multi_function_procedure, SimpleLoop)
{
  /**
   * procedure(int count, int *out) {
   *   out = 1;
   *   int index = 0'
   *   loop {
   *     if (index >= count) {
   *       break;
   *     }
   *     out *= 2;
   *     index += 1;
   *   }
   *   out += 1000;
   * }
   */

  CustomMF_Constant<int> const_1_fn{1};
  CustomMF_Constant<int> const_0_fn{0};
  CustomMF_SI_SI_SO<int, int, bool> greater_or_equal_fn{"greater or equal",
                                                        [](int a, int b) { return a >= b; }};
  CustomMF_SM<int> double_fn{"double", [](int &a) { a *= 2; }};
  CustomMF_SM<int> add_1000_fn{"add 1000", [](int &a) { a += 1000; }};
  CustomMF_SM<int> add_1_fn{"add 1", [](int &a) { a += 1; }};

  MFProcedure procedure;
  MFProcedureBuilder builder{procedure};

  MFVariable *var_count = &builder.add_single_input_parameter<int>("count");
  auto [var_out] = builder.add_call_with_new_variables<1>(const_1_fn);
  var_out->set_name("out");
  auto [var_index] = builder.add_call_with_new_variables<1>(const_0_fn);
  var_index->set_name("index");

  MFProcedureBuilder::Loop loop = builder.add_loop();
  auto [var_condition] = builder.add_call_with_new_variables<1>(greater_or_equal_fn,
                                                                {var_index, var_count});
  var_condition->set_name("condition");
  MFProcedureBuilder::Branch branch = builder.add_branch(*var_condition);
  branch.branch_true.add_destruct(*var_condition);
  branch.branch_true.add_loop_break(loop);
  branch.branch_false.add_destruct(*var_condition);
  builder.set_cursor_after_branch(branch);
  builder.add_call(double_fn, {var_out});
  builder.add_call(add_1_fn, {var_index});
  builder.add_loop_continue(loop);
  builder.set_cursor_after_loop(loop);
  builder.add_call(add_1000_fn, {var_out});
  builder.add_destruct({var_count, var_index});
  builder.add_output_parameter(*var_out);

  MFProcedureExecutor procedure_fn{"Simple Loop", procedure};
  MFParamsBuilder params{procedure_fn, 5};

  Array<int> counts = {4, 3, 7, 6, 4};
  Array<int> results(5, -1);

  params.add_readonly_single_input(counts.as_span());
  params.add_uninitialized_single_output(results.as_mutable_span());

  MFContextBuilder context;
  procedure_fn.call({0, 1, 3, 4}, params, context);

  EXPECT_EQ(results[0], 1016);
  EXPECT_EQ(results[1], 1008);
  EXPECT_EQ(results[2], -1);
  EXPECT_EQ(results[3], 1064);
  EXPECT_EQ(results[4], 1016);
}

TEST(multi_function_procedure, Vectors)
{
  /**
   * procedure(vector<int> v1, vector<int> &v2, vector<int> *v3) {
   *   v1.extend(v2);
   *   int constant = 5;
   *   v2.append(constant);
   *   v2.extend(v1);
   *   int len = sum(v2);
   *   v3 = range(len);
   * }
   */

  CreateRangeFunction create_range_fn;
  ConcatVectorsFunction extend_fn;
  GenericAppendFunction append_fn{CPPType::get<int>()};
  SumVectorFunction sum_elements_fn;
  CustomMF_Constant<int> constant_5_fn{5};

  MFProcedure procedure;
  MFProcedureBuilder builder{procedure};

  MFVariable *var_v1 = &builder.add_input_parameter(MFDataType::ForVector<int>());
  MFVariable *var_v2 = &builder.add_parameter(MFParamType::ForMutableVector(CPPType::get<int>()));
  builder.add_call(extend_fn, {var_v1, var_v2});
  auto [var_constant] = builder.add_call_with_new_variables<1>(constant_5_fn);
  builder.add_call(append_fn, {var_v2, var_constant});
  builder.add_destruct(*var_constant);
  builder.add_call(extend_fn, {var_v2, var_v1});
  auto [var_len] = builder.add_call_with_new_variables<1>(sum_elements_fn, {var_v2});
  auto [var_v3] = builder.add_call_with_new_variables<1>(create_range_fn, {var_len});
  builder.add_destruct({var_v1, var_len});
  builder.add_output_parameter(*var_v3);

  MFProcedureExecutor procedure_fn{"Vectors", procedure};
  MFParamsBuilder params{procedure_fn, 5};

  Array<int> v1 = {5, 2, 3};
  GVectorArray v2{CPPType::get<int>(), 5};
  GVectorArray v3{CPPType::get<int>(), 5};

  int value_10 = 10;
  v2.append(0, &value_10);
  v2.append(4, &value_10);

  params.add_readonly_vector_input(v1.as_span());
  params.add_vector_mutable(v2);
  params.add_vector_output(v3);

  MFContextBuilder context;
  procedure_fn.call({0, 1, 3, 4}, params, context);

  EXPECT_EQ(v2[0].size(), 6);
  EXPECT_EQ(v2[1].size(), 4);
  EXPECT_EQ(v2[2].size(), 0);
  EXPECT_EQ(v2[3].size(), 4);
  EXPECT_EQ(v2[4].size(), 6);

  EXPECT_EQ(v3[0].size(), 35);
  EXPECT_EQ(v3[1].size(), 15);
  EXPECT_EQ(v3[2].size(), 0);
  EXPECT_EQ(v3[3].size(), 15);
  EXPECT_EQ(v3[4].size(), 35);
}

}  // namespace blender::fn::tests
