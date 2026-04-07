; ModuleID = 'MyCompiler'
source_filename = "MyCompiler"

%__StrLit = type { ptr, i32 }
%__iface_fat_ptr = type { ptr, ptr }
%MyStruct = type { i32, i32, i32 }
%MyStruct1 = type { i32 }
%MyStruct2 = type { i32 }
%Tracked = type { i32 }
%NamedThing = type { i32 }
%TypedThing = type { i32, i1 }
%Accumulator = type { i32, i32, i32 }
%Calculator = type { i32 }
%NullableNode = type { i32 }
%StringData = type { ptr, i32 }
%Node = type { i32 }
%Counter = type { i32 }
%Point = type { i32, i32 }

@0 = private unnamed_addr constant [12 x i8] c"%s passed.\0A\00", align 1
@1 = private unnamed_addr constant [40 x i8] c"%s failed expecting '%d' but got '%d'.\0A\00", align 1
@2 = private unnamed_addr constant [40 x i8] c"%s failed expecting '%s' but got '%s'.\0A\00", align 1
@3 = private unnamed_addr constant [6 x i8] c"false\00", align 1
@4 = private unnamed_addr constant [5 x i8] c"true\00", align 1
@5 = private unnamed_addr constant [9 x i8] c"__FILE__\00", align 1
@__FILE__ = private unnamed_addr constant [12 x i8] c"testfile2.c\00", align 1
@6 = private unnamed_addr constant [13 x i8] c"__FUNCTION__\00", align 1
@__FUNCTION__ = private unnamed_addr constant [23 x i8] c"testBuiltinIdentifiers\00", align 1
@7 = private unnamed_addr constant [9 x i8] c"__LINE__\00", align 1
@8 = private unnamed_addr constant [8 x i8] c"InOrder\00", align 1
@9 = private unnamed_addr constant [15 x i8] c"OutOfOrderName\00", align 1
@10 = private unnamed_addr constant [13 x i8] c"MixedInOrder\00", align 1
@11 = private unnamed_addr constant [16 x i8] c"MixedOutOfOrder\00", align 1
@MyEnum.E_ONE = global i8 1
@MyEnum.E_TWO = global i8 2
@MyEnum.E_THREE = global i8 5
@12 = private unnamed_addr constant [11 x i8] c"enum_E_ONE\00", align 1
@13 = private unnamed_addr constant [11 x i8] c"enum_E_TWO\00", align 1
@14 = private unnamed_addr constant [13 x i8] c"enum_E_THREE\00", align 1
@destructorCallCount = global i32 0
@15 = private unnamed_addr constant [23 x i8] c"destructor_called_once\00", align 1
@16 = private unnamed_addr constant [24 x i8] c"destructor_called_twice\00", align 1
@17 = private unnamed_addr constant [14 x i8] c"add_both_args\00", align 1
@18 = private unnamed_addr constant [14 x i8] c"add_default_y\00", align 1
@19 = private unnamed_addr constant [13 x i8] c"sum_all_args\00", align 1
@20 = private unnamed_addr constant [14 x i8] c"sum_default_z\00", align 1
@21 = private unnamed_addr constant [16 x i8] c"sum_defaults_yz\00", align 1
@22 = private unnamed_addr constant [18 x i8] c"multiply_explicit\00", align 1
@23 = private unnamed_addr constant [17 x i8] c"multiply_default\00", align 1
@24 = private unnamed_addr constant [40 x i8] c"%s failed: expected '%s' but got '%s'.\0A\00", align 1
@25 = private unnamed_addr constant [11 x i8] c"typeof_int\00", align 1
@typeof = private unnamed_addr constant [4 x i8] c"int\00", align 1
@26 = private unnamed_addr constant [12 x i8] c"typeof_bool\00", align 1
@typeof.1 = private unnamed_addr constant [5 x i8] c"bool\00", align 1
@27 = private unnamed_addr constant [14 x i8] c"typeof_struct\00", align 1
@typeof.2 = private unnamed_addr constant [11 x i8] c"TypedThing\00", align 1
@28 = private unnamed_addr constant [17 x i8] c"typeof_field_int\00", align 1
@29 = private unnamed_addr constant [18 x i8] c"typeof_field_bool\00", align 1
@30 = private unnamed_addr constant [16 x i8] c"typeof_type_int\00", align 1
@31 = private unnamed_addr constant [17 x i8] c"typeof_type_name\00", align 1
@32 = private unnamed_addr constant [11 x i8] c"nameof_var\00", align 1
@nameof = private unnamed_addr constant [6 x i8] c"myVar\00", align 1
@33 = private unnamed_addr constant [14 x i8] c"nameof_struct\00", align 1
@nameof.3 = private unnamed_addr constant [6 x i8] c"thing\00", align 1
@34 = private unnamed_addr constant [13 x i8] c"nameof_field\00", align 1
@nameof.4 = private unnamed_addr constant [6 x i8] c"value\00", align 1
@35 = private unnamed_addr constant [12 x i8] c"nameof_type\00", align 1
@nameof.5 = private unnamed_addr constant [11 x i8] c"NamedThing\00", align 1
@36 = private unnamed_addr constant [14 x i8] c"namespace_add\00", align 1
@37 = private unnamed_addr constant [19 x i8] c"namespace_multiply\00", align 1
@38 = private unnamed_addr constant [11 x i8] c"global_add\00", align 1
@39 = private unnamed_addr constant [13 x i8] c"no_collision\00", align 1
@40 = private unnamed_addr constant [17 x i8] c"global_using_add\00", align 1
@41 = private unnamed_addr constant [22 x i8] c"global_using_multiply\00", align 1
@42 = private unnamed_addr constant [16 x i8] c"local_using_add\00", align 1
@43 = private unnamed_addr constant [21 x i8] c"local_using_multiply\00", align 1
@44 = private unnamed_addr constant [14 x i8] c"nested_square\00", align 1
@45 = private unnamed_addr constant [12 x i8] c"nested_cube\00", align 1
@46 = private unnamed_addr constant [20 x i8] c"nested_using_square\00", align 1
@47 = private unnamed_addr constant [18 x i8] c"nested_using_cube\00", align 1
@48 = private unnamed_addr constant [27 x i8] c"global_nested_using_square\00", align 1
@49 = private unnamed_addr constant [25 x i8] c"global_nested_using_cube\00", align 1
@50 = private unnamed_addr constant [26 x i8] c"local_nested_using_square\00", align 1
@51 = private unnamed_addr constant [24 x i8] c"local_nested_using_cube\00", align 1
@52 = private unnamed_addr constant [23 x i8] c"local_alias_not_leaked\00", align 1
@53 = private unnamed_addr constant [24 x i8] c"bar calls foo (fwd ref)\00", align 1
@54 = private unnamed_addr constant [10 x i8] c"isEven(0)\00", align 1
@55 = private unnamed_addr constant [10 x i8] c"isEven(4)\00", align 1
@56 = private unnamed_addr constant [9 x i8] c"isOdd(1)\00", align 1
@57 = private unnamed_addr constant [9 x i8] c"isOdd(5)\00", align 1
@58 = private unnamed_addr constant [19 x i8] c"isEven(3) is false\00", align 1
@59 = private unnamed_addr constant [18 x i8] c"isOdd(4) is false\00", align 1
@60 = private unnamed_addr constant [37 x i8] c"runAccumulator (fwd ref to getTotal)\00", align 1
@61 = private unnamed_addr constant [33 x i8] c"member calls fwd func (doubleIt)\00", align 1
@62 = private unnamed_addr constant [33 x i8] c"member calls fwd func (addThree)\00", align 1
@63 = private unnamed_addr constant [27 x i8] c"returnBlock doubleValue(5)\00", align 1
@64 = private unnamed_addr constant [27 x i8] c"returnBlock addValues(3,4)\00", align 1
@65 = private unnamed_addr constant [14 x i8] c"assert 42==42\00", align 1
@66 = private unnamed_addr constant [40 x i8] c"%s failed: expected '%d' but got '%d'.\0A\00", align 1
@67 = private unnamed_addr constant [12 x i8] c"assert 7==7\00", align 1
@68 = private unnamed_addr constant [14 x i8] c"assert 10==10\00", align 1
@69 = private unnamed_addr constant [14 x i8] c"assert 10==99\00", align 1
@70 = private unnamed_addr constant [16 x i8] c"assert_all_pass\00", align 1
@71 = private unnamed_addr constant [18 x i8] c"assert_early_exit\00", align 1
@72 = private unnamed_addr constant [9 x i8] c"i8_value\00", align 1
@73 = private unnamed_addr constant [10 x i8] c"i16_value\00", align 1
@74 = private unnamed_addr constant [10 x i8] c"i32_value\00", align 1
@75 = private unnamed_addr constant [10 x i8] c"i64_value\00", align 1
@76 = private unnamed_addr constant [9 x i8] c"u8_value\00", align 1
@77 = private unnamed_addr constant [10 x i8] c"u16_value\00", align 1
@78 = private unnamed_addr constant [10 x i8] c"u32_value\00", align 1
@79 = private unnamed_addr constant [10 x i8] c"u64_value\00", align 1
@80 = private unnamed_addr constant [7 x i8] c"i8_add\00", align 1
@81 = private unnamed_addr constant [8 x i8] c"i16_add\00", align 1
@82 = private unnamed_addr constant [8 x i8] c"i32_add\00", align 1
@83 = private unnamed_addr constant [7 x i8] c"u8_add\00", align 1
@84 = private unnamed_addr constant [8 x i8] c"u16_add\00", align 1
@85 = private unnamed_addr constant [11 x i8] c"i32_to_int\00", align 1
@86 = private unnamed_addr constant [11 x i8] c"int_to_i32\00", align 1
@87 = private unnamed_addr constant [9 x i8] c"func_i32\00", align 1
@88 = private unnamed_addr constant [9 x i8] c"func_u32\00", align 1
@89 = private unnamed_addr constant [17 x i8] c"int_to_i32_param\00", align 1
@90 = private unnamed_addr constant [20 x i8] c"nc_coalesce_nonnull\00", align 1
@91 = private unnamed_addr constant [17 x i8] c"nc_coalesce_null\00", align 1
@92 = private unnamed_addr constant [18 x i8] c"nc_method_nonnull\00", align 1
@93 = private unnamed_addr constant [15 x i8] c"nc_method_null\00", align 1
@94 = private unnamed_addr constant [6 x i8] c"Alice\00", align 1
@95 = private unnamed_addr constant [8 x i8] c"Unknown\00", align 1
@96 = private unnamed_addr constant [21 x i8] c"nullcoal_nonnull_ptr\00", align 1
@97 = private unnamed_addr constant [18 x i8] c"nullcoal_null_ptr\00", align 1
@98 = private unnamed_addr constant [14 x i8] c"nullcoal_zero\00", align 1
@99 = private unnamed_addr constant [17 x i8] c"nullcoal_nonzero\00", align 1
@100 = private unnamed_addr constant [12 x i8] c"default_int\00", align 1
@101 = private unnamed_addr constant [13 x i8] c"default_bool\00", align 1
@102 = private unnamed_addr constant [12 x i8] c"default_i32\00", align 1
@103 = private unnamed_addr constant [12 x i8] c"default_i64\00", align 1
@104 = private unnamed_addr constant [20 x i8] c"default_struct_num1\00", align 1
@105 = private unnamed_addr constant [20 x i8] c"default_struct_num2\00", align 1
@106 = private unnamed_addr constant [20 x i8] c"default_struct_num3\00", align 1
@StringData_IReadOnlyString_vtable = internal constant [2 x ptr] [ptr @_data_i8Ptr_StringDataPtr_, ptr @_length_i32_StringDataPtr_]
@107 = private unnamed_addr constant [6 x i8] c"hello\00", align 1
@__StrLit_IReadOnlyString_vtable = internal constant [2 x ptr] [ptr @__StrLit.data, ptr @__StrLit.length]
@108 = private unnamed_addr constant [14 x i8] c"string_length\00", align 1
@109 = private unnamed_addr constant [12 x i8] c"string_data\00", align 1
@110 = private unnamed_addr constant [8 x i8] c"world!!\00", align 1
@111 = private unnamed_addr constant [15 x i8] c"string2_length\00", align 1
@112 = private unnamed_addr constant [13 x i8] c"string2_data\00", align 1
@113 = private unnamed_addr constant [15 x i8] c"str_lit_length\00", align 1
@114 = private unnamed_addr constant [13 x i8] c"str_lit_data\00", align 1
@115 = private unnamed_addr constant [15 x i8] c"My test string\00", align 1
@116 = private unnamed_addr constant [20 x i8] c"str_lit_long_length\00", align 1
@117 = private unnamed_addr constant [18 x i8] c"str_lit_long_data\00", align 1
@118 = private unnamed_addr constant [1 x i8] zeroinitializer, align 1
@119 = private unnamed_addr constant [21 x i8] c"str_lit_empty_length\00", align 1
@120 = private unnamed_addr constant [19 x i8] c"str_lit_empty_data\00", align 1
@121 = private unnamed_addr constant [12 x i8] c"line1\0Aline2\00", align 1
@122 = private unnamed_addr constant [22 x i8] c"str_lit_escape_length\00", align 1
@123 = private unnamed_addr constant [6 x i8] c"alpha\00", align 1
@124 = private unnamed_addr constant [6 x i8] c"beta!\00", align 1
@125 = private unnamed_addr constant [23 x i8] c"str_lit_multi_a_length\00", align 1
@126 = private unnamed_addr constant [23 x i8] c"str_lit_multi_b_length\00", align 1
@127 = private unnamed_addr constant [21 x i8] c"str_lit_multi_a_data\00", align 1
@128 = private unnamed_addr constant [21 x i8] c"str_lit_multi_b_data\00", align 1
@129 = private unnamed_addr constant [54 x i8] c"testNewArray failed: arr[%d] expected %d but got %d.\0A\00", align 1
@fmtlit = private unnamed_addr constant [2 x i8] c"(\00", align 1
@fmtlit.6 = private unnamed_addr constant [3 x i8] c", \00", align 1
@fmtlit.7 = private unnamed_addr constant [2 x i8] c")\00", align 1
@130 = private unnamed_addr constant [6 x i8] c"World\00", align 1
@fmtlit.8 = private unnamed_addr constant [7 x i8] c"Hello \00", align 1
@fmtlit.9 = private unnamed_addr constant [2 x i8] c"!\00", align 1
@131 = private unnamed_addr constant [11 x i8] c"fmt_simple\00", align 1
@132 = private unnamed_addr constant [13 x i8] c"Hello World!\00", align 1
@fmtlit.10 = private unnamed_addr constant [10 x i8] c"Count is \00", align 1
@fmtlit.11 = private unnamed_addr constant [2 x i8] c".\00", align 1
@133 = private unnamed_addr constant [8 x i8] c"fmt_int\00", align 1
@134 = private unnamed_addr constant [13 x i8] c"Count is 42.\00", align 1
@135 = private unnamed_addr constant [4 x i8] c"foo\00", align 1
@136 = private unnamed_addr constant [4 x i8] c"bar\00", align 1
@fmtlit.12 = private unnamed_addr constant [6 x i8] c" and \00", align 1
@137 = private unnamed_addr constant [8 x i8] c"fmt_two\00", align 1
@138 = private unnamed_addr constant [12 x i8] c"foo and bar\00", align 1
@139 = private unnamed_addr constant [10 x i8] c"testArray\00", align 1
@140 = private unnamed_addr constant [17 x i8] c"testTotalByValue\00", align 1
@141 = private unnamed_addr constant [19 x i8] c"testTotalByPointer\00", align 1
@142 = private unnamed_addr constant [13 x i8] c"struct1.Read\00", align 1
@143 = private unnamed_addr constant [13 x i8] c"struct2.Read\00", align 1
@144 = private unnamed_addr constant [16 x i8] c"struct1.ReadExt\00", align 1
@145 = private unnamed_addr constant [16 x i8] c"struct2.ReadExt\00", align 1
@146 = private unnamed_addr constant [14 x i8] c"testNewDelete\00", align 1
@147 = private unnamed_addr constant [13 x i8] c"testNewArray\00", align 1
@148 = private unnamed_addr constant [23 x i8] c"testNewWithConstructor\00", align 1
@149 = private unnamed_addr constant [18 x i8] c"All Test Passed.\0A\00", align 1

define internal ptr @__StrLit.data(ptr %self) {
entry:
  %0 = getelementptr inbounds %__StrLit, ptr %self, i32 0, i32 0
  %1 = load ptr, ptr %0, align 8
  ret ptr %1
}

define internal i32 @__StrLit.length(ptr %self) {
entry:
  %0 = getelementptr inbounds %__StrLit, ptr %self, i32 0, i32 1
  %1 = load i32, ptr %0, align 4
  ret i32 %1
}

define %__iface_fat_ptr @"_operator string_IReadOnlyStringPtr_IStringPtr_"(%__iface_fat_ptr %s) {
entry:
  %s1 = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %s, ptr %s1, align 8
  %0 = getelementptr inbounds %__iface_fat_ptr, ptr %s1, i32 0, i32 0
  %1 = load ptr, ptr %0, align 8
  %2 = getelementptr inbounds %__iface_fat_ptr, ptr %s1, i32 0, i32 1
  %3 = load ptr, ptr %2, align 8
  %4 = getelementptr ptr, ptr %1, i32 0
  %5 = load ptr, ptr %4, align 8
  %6 = call %__iface_fat_ptr %5(ptr %3)
  ret %__iface_fat_ptr %6
}

define ptr @"_operator new_U8Ptr_long_"(i64 %size) {
entry:
  %0 = call ptr @malloc(i64 %size)
  ret ptr %0
}

define void @"_operator delete_void_U8Ptr_"(ptr %ptr) {
entry:
  call void @free(ptr %ptr)
  ret void
}

define i32 @_Test_int_charPtri64i64_(ptr %testName, i64 %actual, i64 %expected) {
entry:
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %0 = icmp eq i64 %expected, %actual
  br i1 %0, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @0, ptr %testName)
  ret i32 1

ifResume:                                         ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @1, ptr %testName, i64 %expected, i64 %actual)
  ret i32 0
}

define i32 @_Test_int_charPtrintint_(ptr %testName, i32 %actual, i32 %expected) {
entry:
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %0 = icmp eq i32 %expected, %actual
  br i1 %0, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @0, ptr %testName)
  ret i32 1

ifResume:                                         ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @1, ptr %testName, i32 %expected, i32 %actual)
  ret i32 0
}

define i1 @_TestBool_bool_charPtrbool_(ptr %testName, i1 %actual) {
entry:
  %expected = alloca i1, align 1
  store i1 true, ptr %expected, align 1
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %0 = load i1, ptr %expected, align 1
  %1 = icmp eq i1 %0, %actual
  br i1 %1, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @0, ptr %testName)
  ret i1 true

ifResume:                                         ; preds = %ifCondition
  %2 = load i1, ptr %expected, align 1
  %3 = select i1 %2, ptr @4, ptr @3
  %4 = select i1 %actual, ptr @4, ptr @3
  call void (ptr, ...) @printf(ptr @2, ptr %testName, ptr %3, ptr %4)
  ret i1 false
}

define i1 @_testBuiltinIdentifiers_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @5, ptr @__FILE__, ptr @__FILE__)
  %1 = load i1, ptr %result, align 1
  %2 = and i1 %1, %0
  %tobool = icmp ne i1 %2, false
  store i1 %tobool, ptr %result, align 1
  %3 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @6, ptr @__FUNCTION__, ptr @__FUNCTION__)
  %4 = load i1, ptr %result, align 1
  %5 = and i1 %4, %3
  %tobool1 = icmp ne i1 %5, false
  store i1 %tobool1, ptr %result, align 1
  %6 = call i32 @_Test_int_charPtrintint_(ptr @7, i32 77, i32 65)
  %7 = load i1, ptr %result, align 1
  %8 = zext i1 %7 to i32
  %9 = and i32 %8, %6
  %tobool2 = icmp ne i32 %9, 0
  store i1 %tobool2, ptr %result, align 1
  %10 = load i1, ptr %result, align 1
  ret i1 %10
}

define i32 @_testArray_int__() {
entry:
  %arraySize = alloca i32, align 4
  store i32 30, ptr %arraySize, align 4
  %0 = load i32, ptr %arraySize, align 4
  %array = alloca i32, i32 %0, align 4
  %i = alloca i32, align 4
  store i32 0, ptr %i, align 4
  br label %whileCondition

whileCondition:                                   ; preds = %whileInner, %entry
  %1 = load i32, ptr %i, align 4
  %2 = load i32, ptr %arraySize, align 4
  %3 = icmp slt i32 %1, %2
  br i1 %3, label %whileInner, label %whileResume

whileInner:                                       ; preds = %whileCondition
  %4 = load i32, ptr %i, align 4
  %5 = getelementptr i32, ptr %array, i32 %4
  %6 = load i32, ptr %i, align 4
  store i32 %6, ptr %5, align 4
  %7 = load i32, ptr %i, align 4
  %8 = load i32, ptr %i, align 4
  %9 = add i32 %8, 1
  store i32 %9, ptr %i, align 4
  br label %whileCondition

whileResume:                                      ; preds = %whileCondition
  %sum = alloca i32, align 4
  store i32 0, ptr %sum, align 4
  store i32 0, ptr %i, align 4
  br label %whileCondition1

whileCondition1:                                  ; preds = %whileInner2, %whileResume
  %10 = load i32, ptr %i, align 4
  %11 = load i32, ptr %arraySize, align 4
  %12 = icmp slt i32 %10, %11
  br i1 %12, label %whileInner2, label %whileResume3

whileInner2:                                      ; preds = %whileCondition1
  %13 = load i32, ptr %i, align 4
  %14 = getelementptr i32, ptr %array, i32 %13
  %15 = load i32, ptr %14, align 4
  %16 = load i32, ptr %sum, align 4
  %17 = add i32 %16, %15
  store i32 %17, ptr %sum, align 4
  %18 = load i32, ptr %i, align 4
  %19 = load i32, ptr %i, align 4
  %20 = add i32 %19, 1
  store i32 %20, ptr %i, align 4
  br label %whileCondition1

whileResume3:                                     ; preds = %whileCondition1
  %21 = load i32, ptr %sum, align 4
  ret i32 %21
}

define i32 @_myFunction_int_intintint_(i32 %x, i32 %y, i32 %z) {
entry:
  %0 = add i32 %x, %y
  %1 = add i32 %x, %y
  %2 = add i32 %1, 1
  %3 = mul i32 %0, %2
  %4 = mul i32 %3, 2
  %5 = add i32 %4, %y
  %pair_xy = alloca i32, align 4
  store i32 %5, ptr %pair_xy, align 4
  %6 = load i32, ptr %pair_xy, align 4
  %7 = add i32 %6, %z
  %8 = load i32, ptr %pair_xy, align 4
  %9 = add i32 %8, %z
  %10 = add i32 %9, 1
  %11 = mul i32 %7, %10
  %12 = mul i32 %11, 2
  %13 = add i32 %12, %z
  ret i32 %13
}

define i1 @_testNamedParameters_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_myFunction_int_intintint_(i32 1, i32 2, i32 3)
  %expected = alloca i32, align 4
  store i32 %0, ptr %expected, align 4
  %1 = call i32 @_myFunction_int_intintint_(i32 1, i32 2, i32 3)
  %2 = load i32, ptr %expected, align 4
  %3 = call i32 @_Test_int_charPtrintint_(ptr @8, i32 %1, i32 %2)
  %4 = load i1, ptr %result, align 1
  %5 = zext i1 %4 to i32
  %6 = and i32 %5, %3
  %tobool = icmp ne i32 %6, 0
  store i1 %tobool, ptr %result, align 1
  %7 = call i32 @_myFunction_int_intintint_(i32 1, i32 2, i32 3)
  %8 = load i32, ptr %expected, align 4
  %9 = call i32 @_Test_int_charPtrintint_(ptr @9, i32 %7, i32 %8)
  %10 = load i1, ptr %result, align 1
  %11 = zext i1 %10 to i32
  %12 = and i32 %11, %9
  %tobool1 = icmp ne i32 %12, 0
  store i1 %tobool1, ptr %result, align 1
  %13 = call i32 @_myFunction_int_intintint_(i32 1, i32 2, i32 3)
  %14 = load i32, ptr %expected, align 4
  %15 = call i32 @_Test_int_charPtrintint_(ptr @10, i32 %13, i32 %14)
  %16 = load i1, ptr %result, align 1
  %17 = zext i1 %16 to i32
  %18 = and i32 %17, %15
  %tobool2 = icmp ne i32 %18, 0
  store i1 %tobool2, ptr %result, align 1
  %19 = call i32 @_myFunction_int_intintint_(i32 1, i32 2, i32 3)
  %20 = load i32, ptr %expected, align 4
  %21 = call i32 @_Test_int_charPtrintint_(ptr @11, i32 %19, i32 %20)
  %22 = load i1, ptr %result, align 1
  %23 = zext i1 %22 to i32
  %24 = and i32 %23, %21
  %tobool3 = icmp ne i32 %24, 0
  store i1 %tobool3, ptr %result, align 1
  %25 = load i1, ptr %result, align 1
  ret i1 %25
}

define i1 @_testEnum_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = load i8, ptr @MyEnum.E_ONE, align 1
  %1 = zext i8 %0 to i32
  %2 = call i32 @_Test_int_charPtrintint_(ptr @12, i32 %1, i32 1)
  %3 = load i1, ptr %result, align 1
  %4 = zext i1 %3 to i32
  %5 = and i32 %4, %2
  %tobool = icmp ne i32 %5, 0
  store i1 %tobool, ptr %result, align 1
  %6 = load i8, ptr @MyEnum.E_TWO, align 1
  %7 = zext i8 %6 to i32
  %8 = call i32 @_Test_int_charPtrintint_(ptr @13, i32 %7, i32 2)
  %9 = load i1, ptr %result, align 1
  %10 = zext i1 %9 to i32
  %11 = and i32 %10, %8
  %tobool1 = icmp ne i32 %11, 0
  store i1 %tobool1, ptr %result, align 1
  %12 = load i8, ptr @MyEnum.E_THREE, align 1
  %13 = zext i8 %12 to i32
  %14 = call i32 @_Test_int_charPtrintint_(ptr @14, i32 %13, i32 5)
  %15 = load i1, ptr %result, align 1
  %16 = zext i1 %15 to i32
  %17 = and i32 %16, %14
  %tobool2 = icmp ne i32 %17, 0
  store i1 %tobool2, ptr %result, align 1
  %18 = load i1, ptr %result, align 1
  ret i1 %18
}

define %MyStruct @_MyStruct_MyStruct__() {
entry:
  ret %MyStruct { i32 1, i32 2, i32 3 }
}

define i32 @_TotalByValue_int_MyStruct_(%MyStruct %mystruct) {
entry:
  %0 = extractvalue %MyStruct %mystruct, 0
  %1 = extractvalue %MyStruct %mystruct, 1
  %2 = add i32 %0, %1
  %3 = extractvalue %MyStruct %mystruct, 1
  %4 = add i32 %2, %3
  ret i32 %4
}

define i32 @_TotalByPointer_int_MyStructPtr_(ptr %mystruct) {
entry:
  %0 = load ptr, ptr %mystruct, align 8
  %1 = getelementptr inbounds %MyStruct, ptr %0, i32 0, i32 0
  %2 = load i32, ptr %1, align 4
  %3 = load ptr, ptr %mystruct, align 8
  %4 = getelementptr inbounds %MyStruct, ptr %3, i32 0, i32 1
  %5 = load i32, ptr %4, align 4
  %6 = add i32 %2, %5
  %7 = load ptr, ptr %mystruct, align 8
  %8 = getelementptr inbounds %MyStruct, ptr %7, i32 0, i32 1
  %9 = load i32, ptr %8, align 4
  %10 = add i32 %6, %9
  ret i32 %10
}

define %MyStruct1 @_MyStruct1_MyStruct1__() {
entry:
  ret %MyStruct1 { i32 1 }
}

define i32 @_Read_int_MyStruct1Ptr_(ptr %MyStruct1__) {
entry:
  %0 = getelementptr inbounds %MyStruct1, ptr %MyStruct1__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  ret i32 %1
}

define %MyStruct2 @_MyStruct2_MyStruct2__() {
entry:
  ret %MyStruct2 { i32 2 }
}

define i32 @_Read_int_MyStruct2Ptr_(ptr %MyStruct2__) {
entry:
  %0 = getelementptr inbounds %MyStruct2, ptr %MyStruct2__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  ret i32 %1
}

define i32 @_ReadExt_int_MyStruct1_(%MyStruct1 %my) {
entry:
  %0 = extractvalue %MyStruct1 %my, 0
  ret i32 %0
}

define i32 @_ReadExt_int_MyStruct2_(%MyStruct2 %my) {
entry:
  %0 = extractvalue %MyStruct2 %my, 0
  ret i32 %0
}

define %Tracked @_Tracked_Tracked__() {
entry:
  ret %Tracked zeroinitializer
}

define void @"_~Tracked_void_TrackedPtr_"(ptr %Tracked__) {
entry:
  %0 = load i32, ptr @destructorCallCount, align 4
  %1 = add i32 %0, 1
  store i32 %1, ptr @destructorCallCount, align 4
  ret void
}

define i1 @_testDestructor_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  store i32 0, ptr @destructorCallCount, align 4
  %0 = call %Tracked @_Tracked_Tracked__()
  %t = alloca %Tracked, align 8
  store %Tracked %0, ptr %t, align 4
  call void @"_~Tracked_void_TrackedPtr_"(ptr %t)
  %1 = load i32, ptr @destructorCallCount, align 4
  %2 = call i32 @_Test_int_charPtrintint_(ptr @15, i32 %1, i32 1)
  %3 = load i1, ptr %result, align 1
  %4 = zext i1 %3 to i32
  %5 = and i32 %4, %2
  %tobool = icmp ne i32 %5, 0
  store i1 %tobool, ptr %result, align 1
  %6 = call %Tracked @_Tracked_Tracked__()
  %a = alloca %Tracked, align 8
  store %Tracked %6, ptr %a, align 4
  %7 = call %Tracked @_Tracked_Tracked__()
  %b = alloca %Tracked, align 8
  store %Tracked %7, ptr %b, align 4
  call void @"_~Tracked_void_TrackedPtr_"(ptr %a)
  call void @"_~Tracked_void_TrackedPtr_"(ptr %b)
  %8 = load i32, ptr @destructorCallCount, align 4
  %9 = call i32 @_Test_int_charPtrintint_(ptr @16, i32 %8, i32 3)
  %10 = load i1, ptr %result, align 1
  %11 = zext i1 %10 to i32
  %12 = and i32 %11, %9
  %tobool1 = icmp ne i32 %12, 0
  store i1 %tobool1, ptr %result, align 1
  %13 = load i1, ptr %result, align 1
  ret i1 %13
}

define i32 @_addWithDefault_int_intint_(i32 %x, i32 %y) {
entry:
  %0 = add i32 %x, %y
  ret i32 %0
}

define i32 @_addWithDefault_int_int_(i32 %x) {
entry:
  %0 = call i32 @_addWithDefault_int_intint_(i32 %x, i32 5)
  ret i32 %0
}

define i1 @_testDefaultSingleParam_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_addWithDefault_int_intint_(i32 3, i32 4)
  %1 = call i32 @_Test_int_charPtrintint_(ptr @17, i32 %0, i32 7)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = call i32 @_addWithDefault_int_int_(i32 3)
  %6 = call i32 @_Test_int_charPtrintint_(ptr @18, i32 %5, i32 8)
  %7 = load i1, ptr %result, align 1
  %8 = zext i1 %7 to i32
  %9 = and i32 %8, %6
  %tobool1 = icmp ne i32 %9, 0
  store i1 %tobool1, ptr %result, align 1
  %10 = load i1, ptr %result, align 1
  ret i1 %10
}

define i32 @_sumWithDefaults_int_intintint_(i32 %x, i32 %y, i32 %z) {
entry:
  %0 = add i32 %x, %y
  %1 = add i32 %0, %z
  ret i32 %1
}

define i32 @_sumWithDefaults_int_int_(i32 %x) {
entry:
  %0 = call i32 @_sumWithDefaults_int_intintint_(i32 %x, i32 10, i32 20)
  ret i32 %0
}

define i32 @_sumWithDefaults_int_intint_(i32 %x, i32 %y) {
entry:
  %0 = call i32 @_sumWithDefaults_int_intintint_(i32 %x, i32 %y, i32 20)
  ret i32 %0
}

define i1 @_testDefaultMultipleParams_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_sumWithDefaults_int_intintint_(i32 1, i32 2, i32 3)
  %1 = call i32 @_Test_int_charPtrintint_(ptr @19, i32 %0, i32 6)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = call i32 @_sumWithDefaults_int_intint_(i32 1, i32 2)
  %6 = call i32 @_Test_int_charPtrintint_(ptr @20, i32 %5, i32 23)
  %7 = load i1, ptr %result, align 1
  %8 = zext i1 %7 to i32
  %9 = and i32 %8, %6
  %tobool1 = icmp ne i32 %9, 0
  store i1 %tobool1, ptr %result, align 1
  %10 = call i32 @_sumWithDefaults_int_int_(i32 1)
  %11 = call i32 @_Test_int_charPtrintint_(ptr @21, i32 %10, i32 31)
  %12 = load i1, ptr %result, align 1
  %13 = zext i1 %12 to i32
  %14 = and i32 %13, %11
  %tobool2 = icmp ne i32 %14, 0
  store i1 %tobool2, ptr %result, align 1
  %15 = load i1, ptr %result, align 1
  ret i1 %15
}

define i32 @_multiplyWithDefault_int_intint_(i32 %x, i32 %factor) {
entry:
  %0 = mul i32 %x, %factor
  ret i32 %0
}

define i32 @_multiplyWithDefault_int_int_(i32 %x) {
entry:
  %0 = call i32 @_multiplyWithDefault_int_intint_(i32 %x, i32 2)
  ret i32 %0
}

define i1 @_testDefaultExpression_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_multiplyWithDefault_int_intint_(i32 5, i32 3)
  %1 = call i32 @_Test_int_charPtrintint_(ptr @22, i32 %0, i32 15)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = call i32 @_multiplyWithDefault_int_int_(i32 5)
  %6 = call i32 @_Test_int_charPtrintint_(ptr @23, i32 %5, i32 10)
  %7 = load i1, ptr %result, align 1
  %8 = zext i1 %7 to i32
  %9 = and i32 %8, %6
  %tobool1 = icmp ne i32 %9, 0
  store i1 %tobool1, ptr %result, align 1
  %10 = load i1, ptr %result, align 1
  ret i1 %10
}

define i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr %testName, ptr %actual, ptr %expected) {
entry:
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %0 = call i32 @strcmp(ptr %actual, ptr %expected)
  %1 = icmp eq i32 %0, 0
  br i1 %1, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @0, ptr %testName)
  ret i1 true

ifResume:                                         ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @24, ptr %testName, ptr %expected, ptr %actual)
  ret i1 false
}

define %NamedThing @_NamedThing_NamedThing__() {
entry:
  ret %NamedThing { i32 42 }
}

define %TypedThing @_TypedThing_TypedThing__() {
entry:
  ret %TypedThing zeroinitializer
}

define i1 @_testTypeof_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %i = alloca i32, align 4
  store i32 0, ptr %i, align 4
  %b = alloca i1, align 1
  store i1 false, ptr %b, align 1
  %0 = call %TypedThing @_TypedThing_TypedThing__()
  %t = alloca %TypedThing, align 8
  store %TypedThing %0, ptr %t, align 4
  %1 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @25, ptr @typeof, ptr @typeof)
  %2 = load i1, ptr %result, align 1
  %3 = and i1 %2, %1
  %tobool = icmp ne i1 %3, false
  store i1 %tobool, ptr %result, align 1
  %4 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @26, ptr @typeof.1, ptr @typeof.1)
  %5 = load i1, ptr %result, align 1
  %6 = and i1 %5, %4
  %tobool1 = icmp ne i1 %6, false
  store i1 %tobool1, ptr %result, align 1
  %7 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @27, ptr @typeof.2, ptr @typeof.2)
  %8 = load i1, ptr %result, align 1
  %9 = and i1 %8, %7
  %tobool2 = icmp ne i1 %9, false
  store i1 %tobool2, ptr %result, align 1
  %10 = getelementptr inbounds %TypedThing, ptr %t, i32 0, i32 0
  %11 = load i32, ptr %10, align 4
  %12 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @28, ptr @typeof, ptr @typeof)
  %13 = load i1, ptr %result, align 1
  %14 = and i1 %13, %12
  %tobool3 = icmp ne i1 %14, false
  store i1 %tobool3, ptr %result, align 1
  %15 = getelementptr inbounds %TypedThing, ptr %t, i32 0, i32 1
  %16 = load i1, ptr %15, align 1
  %17 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @29, ptr @typeof.1, ptr @typeof.1)
  %18 = load i1, ptr %result, align 1
  %19 = and i1 %18, %17
  %tobool4 = icmp ne i1 %19, false
  store i1 %tobool4, ptr %result, align 1
  %20 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @30, ptr @typeof, ptr @typeof)
  %21 = load i1, ptr %result, align 1
  %22 = and i1 %21, %20
  %tobool5 = icmp ne i1 %22, false
  store i1 %tobool5, ptr %result, align 1
  %23 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @31, ptr @typeof.2, ptr @typeof.2)
  %24 = load i1, ptr %result, align 1
  %25 = and i1 %24, %23
  %tobool6 = icmp ne i1 %25, false
  store i1 %tobool6, ptr %result, align 1
  %26 = load i1, ptr %result, align 1
  ret i1 %26
}

define i1 @_testNameof_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %myVar = alloca i32, align 4
  store i32 0, ptr %myVar, align 4
  %0 = call %NamedThing @_NamedThing_NamedThing__()
  %thing = alloca %NamedThing, align 8
  store %NamedThing %0, ptr %thing, align 4
  %1 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @32, ptr @nameof, ptr @nameof)
  %2 = load i1, ptr %result, align 1
  %3 = and i1 %2, %1
  %tobool = icmp ne i1 %3, false
  store i1 %tobool, ptr %result, align 1
  %4 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @33, ptr @nameof.3, ptr @nameof.3)
  %5 = load i1, ptr %result, align 1
  %6 = and i1 %5, %4
  %tobool1 = icmp ne i1 %6, false
  store i1 %tobool1, ptr %result, align 1
  %7 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @34, ptr @nameof.4, ptr @nameof.4)
  %8 = load i1, ptr %result, align 1
  %9 = and i1 %8, %7
  %tobool2 = icmp ne i1 %9, false
  store i1 %tobool2, ptr %result, align 1
  %10 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @35, ptr @nameof.5, ptr @nameof.5)
  %11 = load i1, ptr %result, align 1
  %12 = and i1 %11, %10
  %tobool3 = icmp ne i1 %12, false
  store i1 %tobool3, ptr %result, align 1
  %13 = load i1, ptr %result, align 1
  ret i1 %13
}

define i32 @_add_int_intint_(i32 %a, i32 %b) {
entry:
  %0 = add i32 %a, %b
  %1 = add i32 %0, 100
  ret i32 %1
}

define i32 @_MathUtils.add_int_intint_(i32 %a, i32 %b) {
entry:
  %0 = add i32 %a, %b
  ret i32 %0
}

define i32 @_MathUtils.multiply_int_intint_(i32 %a, i32 %b) {
entry:
  %0 = mul i32 %a, %b
  ret i32 %0
}

define i32 @_MathUtils.Advanced.square_int_int_(i32 %x) {
entry:
  %0 = mul i32 %x, %x
  ret i32 %0
}

define i32 @_MathUtils.Advanced.cube_int_int_(i32 %x) {
entry:
  %0 = mul i32 %x, %x
  %1 = mul i32 %0, %x
  ret i32 %1
}

define i1 @_testNamespace_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_MathUtils.add_int_intint_(i32 3, i32 4)
  %1 = call i32 @_Test_int_charPtrintint_(ptr @36, i32 %0, i32 7)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = call i32 @_MathUtils.multiply_int_intint_(i32 3, i32 4)
  %6 = call i32 @_Test_int_charPtrintint_(ptr @37, i32 %5, i32 12)
  %7 = load i1, ptr %result, align 1
  %8 = zext i1 %7 to i32
  %9 = and i32 %8, %6
  %tobool1 = icmp ne i32 %9, 0
  store i1 %tobool1, ptr %result, align 1
  %10 = call i32 @_add_int_intint_(i32 3, i32 4)
  %11 = call i32 @_Test_int_charPtrintint_(ptr @38, i32 %10, i32 107)
  %12 = load i1, ptr %result, align 1
  %13 = zext i1 %12 to i32
  %14 = and i32 %13, %11
  %tobool2 = icmp ne i32 %14, 0
  store i1 %tobool2, ptr %result, align 1
  %15 = call i32 @_MathUtils.add_int_intint_(i32 3, i32 4)
  %16 = call i32 @_add_int_intint_(i32 3, i32 4)
  %17 = icmp ne i32 %15, %16
  %18 = zext i1 %17 to i32
  %19 = call i32 @_Test_int_charPtrintint_(ptr @39, i32 %18, i32 1)
  %20 = load i1, ptr %result, align 1
  %21 = zext i1 %20 to i32
  %22 = and i32 %21, %19
  %tobool3 = icmp ne i32 %22, 0
  store i1 %tobool3, ptr %result, align 1
  %23 = call i32 @_MathUtils.add_int_intint_(i32 3, i32 4)
  %24 = call i32 @_Test_int_charPtrintint_(ptr @40, i32 %23, i32 7)
  %25 = load i1, ptr %result, align 1
  %26 = zext i1 %25 to i32
  %27 = and i32 %26, %24
  %tobool4 = icmp ne i32 %27, 0
  store i1 %tobool4, ptr %result, align 1
  %28 = call i32 @_MathUtils.multiply_int_intint_(i32 3, i32 4)
  %29 = call i32 @_Test_int_charPtrintint_(ptr @41, i32 %28, i32 12)
  %30 = load i1, ptr %result, align 1
  %31 = zext i1 %30 to i32
  %32 = and i32 %31, %29
  %tobool5 = icmp ne i32 %32, 0
  store i1 %tobool5, ptr %result, align 1
  %33 = load i1, ptr %result, align 1
  ret i1 %33
}

define i1 @_testLocalUsing_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_MathUtils.add_int_intint_(i32 2, i32 3)
  %1 = call i32 @_Test_int_charPtrintint_(ptr @42, i32 %0, i32 5)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = call i32 @_MathUtils.multiply_int_intint_(i32 2, i32 3)
  %6 = call i32 @_Test_int_charPtrintint_(ptr @43, i32 %5, i32 6)
  %7 = load i1, ptr %result, align 1
  %8 = zext i1 %7 to i32
  %9 = and i32 %8, %6
  %tobool1 = icmp ne i32 %9, 0
  store i1 %tobool1, ptr %result, align 1
  %10 = load i1, ptr %result, align 1
  ret i1 %10
}

define i1 @_testNestedNamespace_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_MathUtils.Advanced.square_int_int_(i32 4)
  %1 = call i32 @_Test_int_charPtrintint_(ptr @44, i32 %0, i32 16)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = call i32 @_MathUtils.Advanced.cube_int_int_(i32 3)
  %6 = call i32 @_Test_int_charPtrintint_(ptr @45, i32 %5, i32 27)
  %7 = load i1, ptr %result, align 1
  %8 = zext i1 %7 to i32
  %9 = and i32 %8, %6
  %tobool1 = icmp ne i32 %9, 0
  store i1 %tobool1, ptr %result, align 1
  %10 = call i32 @_MathUtils.Advanced.square_int_int_(i32 5)
  %11 = call i32 @_Test_int_charPtrintint_(ptr @46, i32 %10, i32 25)
  %12 = load i1, ptr %result, align 1
  %13 = zext i1 %12 to i32
  %14 = and i32 %13, %11
  %tobool2 = icmp ne i32 %14, 0
  store i1 %tobool2, ptr %result, align 1
  %15 = call i32 @_MathUtils.Advanced.cube_int_int_(i32 2)
  %16 = call i32 @_Test_int_charPtrintint_(ptr @47, i32 %15, i32 8)
  %17 = load i1, ptr %result, align 1
  %18 = zext i1 %17 to i32
  %19 = and i32 %18, %16
  %tobool3 = icmp ne i32 %19, 0
  store i1 %tobool3, ptr %result, align 1
  %20 = load i1, ptr %result, align 1
  ret i1 %20
}

define i1 @_testNestedUsing_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_MathUtils.Advanced.square_int_int_(i32 4)
  %1 = call i32 @_Test_int_charPtrintint_(ptr @48, i32 %0, i32 16)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = call i32 @_MathUtils.Advanced.cube_int_int_(i32 3)
  %6 = call i32 @_Test_int_charPtrintint_(ptr @49, i32 %5, i32 27)
  %7 = load i1, ptr %result, align 1
  %8 = zext i1 %7 to i32
  %9 = and i32 %8, %6
  %tobool1 = icmp ne i32 %9, 0
  store i1 %tobool1, ptr %result, align 1
  %10 = call i32 @_MathUtils.Advanced.square_int_int_(i32 6)
  %11 = call i32 @_Test_int_charPtrintint_(ptr @50, i32 %10, i32 36)
  %12 = load i1, ptr %result, align 1
  %13 = zext i1 %12 to i32
  %14 = and i32 %13, %11
  %tobool2 = icmp ne i32 %14, 0
  store i1 %tobool2, ptr %result, align 1
  %15 = call i32 @_MathUtils.Advanced.cube_int_int_(i32 4)
  %16 = call i32 @_Test_int_charPtrintint_(ptr @51, i32 %15, i32 64)
  %17 = load i1, ptr %result, align 1
  %18 = zext i1 %17 to i32
  %19 = and i32 %18, %16
  %tobool3 = icmp ne i32 %19, 0
  store i1 %tobool3, ptr %result, align 1
  %20 = load i1, ptr %result, align 1
  ret i1 %20
}

define i1 @_testLocalUsingScoped_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_MathUtils.add_int_intint_(i32 1, i32 2)
  %1 = call i32 @_Test_int_charPtrintint_(ptr @52, i32 %0, i32 3)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = load i1, ptr %result, align 1
  ret i1 %5
}

define i32 @_bar_int__() {
entry:
  %0 = call i32 @_foo_int__()
  ret i32 %0
}

define i32 @_foo_int__() {
entry:
  ret i32 42
}

define i1 @_testForwardFunction_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_bar_int__()
  %1 = call i32 @_Test_int_charPtrintint_(ptr @53, i32 %0, i32 42)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = load i1, ptr %result, align 1
  ret i1 %5
}

define i1 @_isEven_bool_int_(i32 %n) {
entry:
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %0 = icmp eq i32 %n, 0
  br i1 %0, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  ret i1 true

ifResume:                                         ; preds = %ifCondition
  %1 = sub i32 %n, 1
  %2 = call i1 @_isOdd_bool_int_(i32 %1)
  ret i1 %2
}

define i1 @_isOdd_bool_int_(i32 %n) {
entry:
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %0 = icmp eq i32 %n, 0
  br i1 %0, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  ret i1 false

ifResume:                                         ; preds = %ifCondition
  %1 = sub i32 %n, 1
  %2 = call i1 @_isEven_bool_int_(i32 %1)
  ret i1 %2
}

define i1 @_testMutualRecursion_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i1 @_isEven_bool_int_(i32 0)
  %1 = zext i1 %0 to i32
  %2 = call i32 @_Test_int_charPtrintint_(ptr @54, i32 %1, i32 1)
  %3 = load i1, ptr %result, align 1
  %4 = zext i1 %3 to i32
  %5 = and i32 %4, %2
  %tobool = icmp ne i32 %5, 0
  store i1 %tobool, ptr %result, align 1
  %6 = call i1 @_isEven_bool_int_(i32 4)
  %7 = zext i1 %6 to i32
  %8 = call i32 @_Test_int_charPtrintint_(ptr @55, i32 %7, i32 1)
  %9 = load i1, ptr %result, align 1
  %10 = zext i1 %9 to i32
  %11 = and i32 %10, %8
  %tobool1 = icmp ne i32 %11, 0
  store i1 %tobool1, ptr %result, align 1
  %12 = call i1 @_isOdd_bool_int_(i32 1)
  %13 = zext i1 %12 to i32
  %14 = call i32 @_Test_int_charPtrintint_(ptr @56, i32 %13, i32 1)
  %15 = load i1, ptr %result, align 1
  %16 = zext i1 %15 to i32
  %17 = and i32 %16, %14
  %tobool2 = icmp ne i32 %17, 0
  store i1 %tobool2, ptr %result, align 1
  %18 = call i1 @_isOdd_bool_int_(i32 5)
  %19 = zext i1 %18 to i32
  %20 = call i32 @_Test_int_charPtrintint_(ptr @57, i32 %19, i32 1)
  %21 = load i1, ptr %result, align 1
  %22 = zext i1 %21 to i32
  %23 = and i32 %22, %20
  %tobool3 = icmp ne i32 %23, 0
  store i1 %tobool3, ptr %result, align 1
  %24 = call i1 @_isEven_bool_int_(i32 3)
  %25 = zext i1 %24 to i32
  %26 = call i32 @_Test_int_charPtrintint_(ptr @58, i32 %25, i32 0)
  %27 = load i1, ptr %result, align 1
  %28 = zext i1 %27 to i32
  %29 = and i32 %28, %26
  %tobool4 = icmp ne i32 %29, 0
  store i1 %tobool4, ptr %result, align 1
  %30 = call i1 @_isOdd_bool_int_(i32 4)
  %31 = zext i1 %30 to i32
  %32 = call i32 @_Test_int_charPtrintint_(ptr @59, i32 %31, i32 0)
  %33 = load i1, ptr %result, align 1
  %34 = zext i1 %33 to i32
  %35 = and i32 %34, %32
  %tobool5 = icmp ne i32 %35, 0
  store i1 %tobool5, ptr %result, align 1
  %36 = load i1, ptr %result, align 1
  ret i1 %36
}

define i32 @_runAccumulator_int__() {
entry:
  %0 = call i32 @_getTotal_int__()
  ret i32 %0
}

define %Accumulator @_Accumulator_Accumulator__() {
entry:
  ret %Accumulator { i32 10, i32 20, i32 30 }
}

define i32 @_Total_int_AccumulatorPtr_(ptr %Accumulator__) {
entry:
  %0 = getelementptr inbounds %Accumulator, ptr %Accumulator__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  %2 = getelementptr inbounds %Accumulator, ptr %Accumulator__, i32 0, i32 1
  %3 = load i32, ptr %2, align 4
  %4 = add i32 %1, %3
  %5 = getelementptr inbounds %Accumulator, ptr %Accumulator__, i32 0, i32 2
  %6 = load i32, ptr %5, align 4
  %7 = add i32 %4, %6
  ret i32 %7
}

define i32 @_getTotal_int__() {
entry:
  %0 = call %Accumulator @_Accumulator_Accumulator__()
  %a = alloca %Accumulator, align 8
  store %Accumulator %0, ptr %a, align 4
  %1 = call i32 @_Total_int_AccumulatorPtr_(ptr %a)
  ret i32 %1
}

define i1 @_testForwardFunctionWithStruct_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_runAccumulator_int__()
  %1 = call i32 @_Test_int_charPtrintint_(ptr @60, i32 %0, i32 60)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = load i1, ptr %result, align 1
  ret i1 %5
}

define %Calculator @_Calculator_Calculator__() {
entry:
  ret %Calculator { i32 10 }
}

define i32 @_ComputeDouble_int_CalculatorPtr_(ptr %Calculator__) {
entry:
  %0 = call i32 @_doubleIt_int_CalculatorPtr_(ptr %Calculator__)
  ret i32 %0
}

define i32 @_ComputeSum_int_CalculatorPtr_(ptr %Calculator__) {
entry:
  %0 = call i32 @_addThree_int_CalculatorPtrintint_(ptr %Calculator__, i32 5, i32 3)
  ret i32 %0
}

define i32 @_doubleIt_int_CalculatorPtr_(ptr %Calculator__) {
entry:
  %0 = getelementptr inbounds %Calculator, ptr %Calculator__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  %2 = mul i32 %1, 2
  ret i32 %2
}

define i32 @_addThree_int_CalculatorPtrintint_(ptr %Calculator__, i32 %b, i32 %c) {
entry:
  %0 = getelementptr inbounds %Calculator, ptr %Calculator__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  %2 = add i32 %1, %b
  %3 = add i32 %2, %c
  ret i32 %3
}

define i1 @_testForwardInStruct_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call %Calculator @_Calculator_Calculator__()
  %c = alloca %Calculator, align 8
  store %Calculator %0, ptr %c, align 4
  %1 = call i32 @_ComputeDouble_int_CalculatorPtr_(ptr %c)
  %2 = call i32 @_Test_int_charPtrintint_(ptr @61, i32 %1, i32 20)
  %3 = load i1, ptr %result, align 1
  %4 = zext i1 %3 to i32
  %5 = and i32 %4, %2
  %tobool = icmp ne i32 %5, 0
  store i1 %tobool, ptr %result, align 1
  %6 = call i32 @_ComputeSum_int_CalculatorPtr_(ptr %c)
  %7 = call i32 @_Test_int_charPtrintint_(ptr @62, i32 %6, i32 18)
  %8 = load i1, ptr %result, align 1
  %9 = zext i1 %8 to i32
  %10 = and i32 %9, %7
  %tobool1 = icmp ne i32 %10, 0
  store i1 %tobool1, ptr %result, align 1
  %11 = load i1, ptr %result, align 1
  ret i1 %11
}

define i32 @_getDoubled_int_int_(i32 %x) {
entry:
  %x1 = alloca i32, align 4
  store i32 %x, ptr %x1, align 4
  %0 = load i32, ptr %x1, align 4
  %1 = mul i32 %0, 2
  ret i32 %1
}

define i32 @_getSum_int_intint_(i32 %a, i32 %b) {
entry:
  %a1 = alloca i32, align 4
  store i32 %a, ptr %a1, align 4
  %b2 = alloca i32, align 4
  store i32 %b, ptr %b2, align 4
  %0 = load i32, ptr %a1, align 4
  %1 = load i32, ptr %b2, align 4
  %2 = add i32 %0, %1
  ret i32 %2
}

define i1 @_testReturnBlock_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i32 @_getDoubled_int_int_(i32 5)
  %1 = call i32 @_Test_int_charPtrintint_(ptr @63, i32 %0, i32 10)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = call i32 @_getSum_int_intint_(i32 3, i32 4)
  %6 = call i32 @_Test_int_charPtrintint_(ptr @64, i32 %5, i32 7)
  %7 = load i1, ptr %result, align 1
  %8 = zext i1 %7 to i32
  %9 = and i32 %8, %6
  %tobool1 = icmp ne i32 %9, 0
  store i1 %tobool1, ptr %result, align 1
  %10 = load i1, ptr %result, align 1
  ret i1 %10
}

define i1 @_testAssertPass_bool__() {
entry:
  %actual = alloca i32, align 4
  store i32 42, ptr %actual, align 4
  %expected = alloca i32, align 4
  store i32 42, ptr %expected, align 4
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %0 = load i32, ptr %actual, align 4
  %1 = load i32, ptr %expected, align 4
  %2 = icmp ne i32 %0, %1
  br i1 %2, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  %3 = load i32, ptr %expected, align 4
  %4 = load i32, ptr %actual, align 4
  call void (ptr, ...) @printf(ptr @66, ptr @65, i32 %3, i32 %4)
  ret i1 false

ifResume:                                         ; preds = %ifCondition
  %actual1 = alloca i32, align 4
  store i32 7, ptr %actual1, align 4
  %expected2 = alloca i32, align 4
  store i32 7, ptr %expected2, align 4
  br label %ifCondition3

ifCondition3:                                     ; preds = %ifResume
  %5 = load i32, ptr %actual1, align 4
  %6 = load i32, ptr %expected2, align 4
  %7 = icmp ne i32 %5, %6
  br i1 %7, label %ifTrue4, label %ifResume5

ifTrue4:                                          ; preds = %ifCondition3
  %8 = load i32, ptr %expected2, align 4
  %9 = load i32, ptr %actual1, align 4
  call void (ptr, ...) @printf(ptr @66, ptr @67, i32 %8, i32 %9)
  ret i1 false

ifResume5:                                        ; preds = %ifCondition3
  ret i1 true
}

define i1 @_testAssertFail_bool__() {
entry:
  %actual = alloca i32, align 4
  store i32 10, ptr %actual, align 4
  %expected = alloca i32, align 4
  store i32 10, ptr %expected, align 4
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %0 = load i32, ptr %actual, align 4
  %1 = load i32, ptr %expected, align 4
  %2 = icmp ne i32 %0, %1
  br i1 %2, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  %3 = load i32, ptr %expected, align 4
  %4 = load i32, ptr %actual, align 4
  call void (ptr, ...) @printf(ptr @66, ptr @68, i32 %3, i32 %4)
  ret i1 false

ifResume:                                         ; preds = %ifCondition
  %actual1 = alloca i32, align 4
  store i32 10, ptr %actual1, align 4
  %expected2 = alloca i32, align 4
  store i32 99, ptr %expected2, align 4
  br label %ifCondition3

ifCondition3:                                     ; preds = %ifResume
  %5 = load i32, ptr %actual1, align 4
  %6 = load i32, ptr %expected2, align 4
  %7 = icmp ne i32 %5, %6
  br i1 %7, label %ifTrue4, label %ifResume5

ifTrue4:                                          ; preds = %ifCondition3
  %8 = load i32, ptr %expected2, align 4
  %9 = load i32, ptr %actual1, align 4
  call void (ptr, ...) @printf(ptr @66, ptr @69, i32 %8, i32 %9)
  ret i1 false

ifResume5:                                        ; preds = %ifCondition3
  ret i1 true
}

define i1 @_testAssertReturnBlock_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call i1 @_testAssertPass_bool__()
  %1 = call i1 @_TestBool_bool_charPtrbool_(ptr @70, i1 %0)
  %2 = load i1, ptr %result, align 1
  %3 = and i1 %2, %1
  %tobool = icmp ne i1 %3, false
  store i1 %tobool, ptr %result, align 1
  %4 = call i1 @_testAssertFail_bool__()
  %5 = zext i1 %4 to i32
  %6 = call i32 @_Test_int_charPtrintint_(ptr @71, i32 %5, i32 0)
  %7 = load i1, ptr %result, align 1
  %8 = zext i1 %7 to i32
  %9 = and i32 %8, %6
  %tobool1 = icmp ne i32 %9, 0
  store i1 %tobool1, ptr %result, align 1
  %10 = load i1, ptr %result, align 1
  ret i1 %10
}

define i32 @_sumI32_int_i32i32_(i32 %a, i32 %b) {
entry:
  %0 = add i32 %a, %b
  ret i32 %0
}

define i32 @_sumU32_int_u32u32_(i32 %a, i32 %b) {
entry:
  %0 = add i32 %a, %b
  ret i32 %0
}

define i1 @_testExplicitIntTypes_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %s8 = alloca i8, align 1
  store i8 100, ptr %s8, align 1
  %s16 = alloca i16, align 2
  store i16 1000, ptr %s16, align 2
  %s32 = alloca i32, align 4
  store i32 100000, ptr %s32, align 4
  %s64 = alloca i64, align 8
  store i64 200000, ptr %s64, align 4
  %0 = load i8, ptr %s8, align 1
  %1 = zext i8 %0 to i32
  %2 = call i32 @_Test_int_charPtrintint_(ptr @72, i32 %1, i32 100)
  %3 = load i1, ptr %result, align 1
  %4 = zext i1 %3 to i32
  %5 = and i32 %4, %2
  %tobool = icmp ne i32 %5, 0
  store i1 %tobool, ptr %result, align 1
  %6 = load i16, ptr %s16, align 2
  %7 = zext i16 %6 to i32
  %8 = call i32 @_Test_int_charPtrintint_(ptr @73, i32 %7, i32 1000)
  %9 = load i1, ptr %result, align 1
  %10 = zext i1 %9 to i32
  %11 = and i32 %10, %8
  %tobool1 = icmp ne i32 %11, 0
  store i1 %tobool1, ptr %result, align 1
  %12 = load i32, ptr %s32, align 4
  %13 = call i32 @_Test_int_charPtrintint_(ptr @74, i32 %12, i32 100000)
  %14 = load i1, ptr %result, align 1
  %15 = zext i1 %14 to i32
  %16 = and i32 %15, %13
  %tobool2 = icmp ne i32 %16, 0
  store i1 %tobool2, ptr %result, align 1
  %17 = load i64, ptr %s64, align 4
  %18 = call i32 @_Test_int_charPtri64i64_(ptr @75, i64 %17, i64 200000)
  %19 = load i1, ptr %result, align 1
  %20 = zext i1 %19 to i32
  %21 = and i32 %20, %18
  %tobool3 = icmp ne i32 %21, 0
  store i1 %tobool3, ptr %result, align 1
  %u8v = alloca i8, align 1
  store i8 -56, ptr %u8v, align 1
  %u16v = alloca i16, align 2
  store i16 2000, ptr %u16v, align 2
  %u32v = alloca i32, align 4
  store i32 300000, ptr %u32v, align 4
  %u64v = alloca i64, align 8
  store i64 400000, ptr %u64v, align 4
  %22 = load i8, ptr %u8v, align 1
  %23 = zext i8 %22 to i32
  %24 = call i32 @_Test_int_charPtrintint_(ptr @76, i32 %23, i32 200)
  %25 = load i1, ptr %result, align 1
  %26 = zext i1 %25 to i32
  %27 = and i32 %26, %24
  %tobool4 = icmp ne i32 %27, 0
  store i1 %tobool4, ptr %result, align 1
  %28 = load i16, ptr %u16v, align 2
  %29 = zext i16 %28 to i32
  %30 = call i32 @_Test_int_charPtrintint_(ptr @77, i32 %29, i32 2000)
  %31 = load i1, ptr %result, align 1
  %32 = zext i1 %31 to i32
  %33 = and i32 %32, %30
  %tobool5 = icmp ne i32 %33, 0
  store i1 %tobool5, ptr %result, align 1
  %34 = load i32, ptr %u32v, align 4
  %35 = call i32 @_Test_int_charPtrintint_(ptr @78, i32 %34, i32 300000)
  %36 = load i1, ptr %result, align 1
  %37 = zext i1 %36 to i32
  %38 = and i32 %37, %35
  %tobool6 = icmp ne i32 %38, 0
  store i1 %tobool6, ptr %result, align 1
  %39 = load i64, ptr %u64v, align 4
  %40 = call i32 @_Test_int_charPtri64i64_(ptr @79, i64 %39, i64 400000)
  %41 = load i1, ptr %result, align 1
  %42 = zext i1 %41 to i32
  %43 = and i32 %42, %40
  %tobool7 = icmp ne i32 %43, 0
  store i1 %tobool7, ptr %result, align 1
  %s8a = alloca i8, align 1
  store i8 50, ptr %s8a, align 1
  %u8a = alloca i8, align 1
  store i8 100, ptr %u8a, align 1
  %44 = load i8, ptr %s8a, align 1
  %45 = load i8, ptr %s8a, align 1
  %46 = add i8 %44, %45
  %47 = zext i8 %46 to i32
  %48 = call i32 @_Test_int_charPtrintint_(ptr @80, i32 %47, i32 100)
  %49 = load i1, ptr %result, align 1
  %50 = zext i1 %49 to i32
  %51 = and i32 %50, %48
  %tobool8 = icmp ne i32 %51, 0
  store i1 %tobool8, ptr %result, align 1
  %52 = load i16, ptr %s16, align 2
  %53 = load i16, ptr %s16, align 2
  %54 = add i16 %52, %53
  %55 = zext i16 %54 to i32
  %56 = call i32 @_Test_int_charPtrintint_(ptr @81, i32 %55, i32 2000)
  %57 = load i1, ptr %result, align 1
  %58 = zext i1 %57 to i32
  %59 = and i32 %58, %56
  %tobool9 = icmp ne i32 %59, 0
  store i1 %tobool9, ptr %result, align 1
  %60 = load i32, ptr %s32, align 4
  %61 = load i32, ptr %s32, align 4
  %62 = add i32 %60, %61
  %63 = call i32 @_Test_int_charPtrintint_(ptr @82, i32 %62, i32 200000)
  %64 = load i1, ptr %result, align 1
  %65 = zext i1 %64 to i32
  %66 = and i32 %65, %63
  %tobool10 = icmp ne i32 %66, 0
  store i1 %tobool10, ptr %result, align 1
  %67 = load i8, ptr %u8a, align 1
  %68 = load i8, ptr %u8a, align 1
  %69 = add i8 %67, %68
  %70 = zext i8 %69 to i32
  %71 = call i32 @_Test_int_charPtrintint_(ptr @83, i32 %70, i32 200)
  %72 = load i1, ptr %result, align 1
  %73 = zext i1 %72 to i32
  %74 = and i32 %73, %71
  %tobool11 = icmp ne i32 %74, 0
  store i1 %tobool11, ptr %result, align 1
  %75 = load i16, ptr %u16v, align 2
  %76 = load i16, ptr %u16v, align 2
  %77 = add i16 %75, %76
  %78 = zext i16 %77 to i32
  %79 = call i32 @_Test_int_charPtrintint_(ptr @84, i32 %78, i32 4000)
  %80 = load i1, ptr %result, align 1
  %81 = zext i1 %80 to i32
  %82 = and i32 %81, %79
  %tobool12 = icmp ne i32 %82, 0
  store i1 %tobool12, ptr %result, align 1
  %83 = load i32, ptr %s32, align 4
  %ci = alloca i32, align 4
  store i32 %83, ptr %ci, align 4
  %84 = load i32, ptr %ci, align 4
  %85 = call i32 @_Test_int_charPtrintint_(ptr @85, i32 %84, i32 100000)
  %86 = load i1, ptr %result, align 1
  %87 = zext i1 %86 to i32
  %88 = and i32 %87, %85
  %tobool13 = icmp ne i32 %88, 0
  store i1 %tobool13, ptr %result, align 1
  %89 = load i32, ptr %ci, align 4
  %ia = alloca i32, align 4
  store i32 %89, ptr %ia, align 4
  %90 = load i32, ptr %ia, align 4
  %91 = call i32 @_Test_int_charPtrintint_(ptr @86, i32 %90, i32 100000)
  %92 = load i1, ptr %result, align 1
  %93 = zext i1 %92 to i32
  %94 = and i32 %93, %91
  %tobool14 = icmp ne i32 %94, 0
  store i1 %tobool14, ptr %result, align 1
  %95 = call i32 @_sumI32_int_i32i32_(i32 40, i32 2)
  %96 = call i32 @_Test_int_charPtrintint_(ptr @87, i32 %95, i32 42)
  %97 = load i1, ptr %result, align 1
  %98 = zext i1 %97 to i32
  %99 = and i32 %98, %96
  %tobool15 = icmp ne i32 %99, 0
  store i1 %tobool15, ptr %result, align 1
  %100 = call i32 @_sumU32_int_u32u32_(i32 40, i32 2)
  %101 = call i32 @_Test_int_charPtrintint_(ptr @88, i32 %100, i32 42)
  %102 = load i1, ptr %result, align 1
  %103 = zext i1 %102 to i32
  %104 = and i32 %103, %101
  %tobool16 = icmp ne i32 %104, 0
  store i1 %tobool16, ptr %result, align 1
  %x = alloca i32, align 4
  store i32 10, ptr %x, align 4
  %105 = load i32, ptr %x, align 4
  %106 = call i32 @_sumI32_int_i32i32_(i32 %105, i32 5)
  %107 = call i32 @_Test_int_charPtrintint_(ptr @89, i32 %106, i32 15)
  %108 = load i1, ptr %result, align 1
  %109 = zext i1 %108 to i32
  %110 = and i32 %109, %107
  %tobool17 = icmp ne i32 %110, 0
  store i1 %tobool17, ptr %result, align 1
  %111 = load i1, ptr %result, align 1
  ret i1 %111
}

define %NullableNode @_NullableNode_NullableNode__() {
entry:
  ret %NullableNode { i32 99 }
}

define i32 @_Read_int_NullableNodePtr_(ptr %NullableNode__) {
entry:
  %0 = getelementptr inbounds %NullableNode, ptr %NullableNode__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  ret i32 %1
}

define i32 @_readNodeValueNC_int_NullableNodePtr_(ptr %node) {
entry:
  %0 = alloca i32, align 4
  %1 = icmp ne ptr %node, null
  br i1 %1, label %nc_access, label %nc_null

nc_null:                                          ; preds = %entry
  store i32 0, ptr %0, align 4
  br label %nc_resume

nc_access:                                        ; preds = %entry
  %2 = getelementptr inbounds %NullableNode, ptr %node, i32 0, i32 0
  %3 = load i32, ptr %2, align 4
  store i32 %3, ptr %0, align 4
  br label %nc_resume

nc_resume:                                        ; preds = %nc_null, %nc_access
  %4 = load i32, ptr %0, align 4
  %5 = alloca i32, align 4
  %tobool = icmp ne i32 %4, 0
  br i1 %tobool, label %nullcoal_notnull, label %nullcoal_null

nullcoal_null:                                    ; preds = %nc_resume
  store i32 255, ptr %5, align 4
  br label %nullcoal_resume

nullcoal_notnull:                                 ; preds = %nc_resume
  store i32 %4, ptr %5, align 4
  br label %nullcoal_resume

nullcoal_resume:                                  ; preds = %nullcoal_null, %nullcoal_notnull
  %6 = load i32, ptr %5, align 4
  ret i32 %6
}

define i32 @_callNodeRead_int_NullableNodePtr_(ptr %node) {
entry:
  %0 = alloca i32, align 4
  %1 = icmp ne ptr %node, null
  br i1 %1, label %nc_access, label %nc_null

nc_null:                                          ; preds = %entry
  store i32 0, ptr %0, align 4
  br label %nc_resume

nc_access:                                        ; preds = %entry
  %2 = call i32 @_Read_int_NullableNodePtr_(ptr %node)
  store i32 %2, ptr %0, align 4
  br label %nc_resume

nc_resume:                                        ; preds = %nc_null, %nc_access
  %3 = load i32, ptr %0, align 4
  %4 = alloca i32, align 4
  %tobool = icmp ne i32 %3, 0
  br i1 %tobool, label %nullcoal_notnull, label %nullcoal_null

nullcoal_null:                                    ; preds = %nc_resume
  store i32 255, ptr %4, align 4
  br label %nullcoal_resume

nullcoal_notnull:                                 ; preds = %nc_resume
  store i32 %3, ptr %4, align 4
  br label %nullcoal_resume

nullcoal_resume:                                  ; preds = %nullcoal_null, %nullcoal_notnull
  %5 = load i32, ptr %4, align 4
  ret i32 %5
}

define i1 @_testNullConditional_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call %NullableNode @_NullableNode_NullableNode__()
  %n = alloca %NullableNode, align 8
  store %NullableNode %0, ptr %n, align 4
  %p = alloca ptr, align 8
  store ptr %n, ptr %p, align 8
  %np = alloca ptr, align 8
  store ptr null, ptr %np, align 8
  %1 = load ptr, ptr %p, align 8
  %2 = call i32 @_readNodeValueNC_int_NullableNodePtr_(ptr %1)
  %3 = call i32 @_Test_int_charPtrintint_(ptr @90, i32 %2, i32 99)
  %4 = load i1, ptr %result, align 1
  %5 = zext i1 %4 to i32
  %6 = and i32 %5, %3
  %tobool = icmp ne i32 %6, 0
  store i1 %tobool, ptr %result, align 1
  %7 = load ptr, ptr %np, align 8
  %8 = call i32 @_readNodeValueNC_int_NullableNodePtr_(ptr %7)
  %9 = call i32 @_Test_int_charPtrintint_(ptr @91, i32 %8, i32 255)
  %10 = load i1, ptr %result, align 1
  %11 = zext i1 %10 to i32
  %12 = and i32 %11, %9
  %tobool1 = icmp ne i32 %12, 0
  store i1 %tobool1, ptr %result, align 1
  %13 = load ptr, ptr %p, align 8
  %14 = call i32 @_callNodeRead_int_NullableNodePtr_(ptr %13)
  %15 = call i32 @_Test_int_charPtrintint_(ptr @92, i32 %14, i32 99)
  %16 = load i1, ptr %result, align 1
  %17 = zext i1 %16 to i32
  %18 = and i32 %17, %15
  %tobool2 = icmp ne i32 %18, 0
  store i1 %tobool2, ptr %result, align 1
  %19 = load ptr, ptr %np, align 8
  %20 = call i32 @_callNodeRead_int_NullableNodePtr_(ptr %19)
  %21 = call i32 @_Test_int_charPtrintint_(ptr @93, i32 %20, i32 255)
  %22 = load i1, ptr %result, align 1
  %23 = zext i1 %22 to i32
  %24 = and i32 %23, %21
  %tobool3 = icmp ne i32 %24, 0
  store i1 %tobool3, ptr %result, align 1
  %25 = load i1, ptr %result, align 1
  ret i1 %25
}

define ptr @_tryGetName_charPtr_bool_(i1 %found) {
entry:
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  br i1 %found, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  ret ptr @94

ifResume:                                         ; preds = %ifCondition
  ret ptr null
}

define i1 @_testNullCoalescing_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %0 = call ptr @_tryGetName_charPtr_bool_(i1 true)
  %1 = alloca ptr, align 8
  %2 = icmp ne ptr %0, null
  br i1 %2, label %nullcoal_notnull, label %nullcoal_null

nullcoal_null:                                    ; preds = %entry
  store ptr @95, ptr %1, align 8
  br label %nullcoal_resume

nullcoal_notnull:                                 ; preds = %entry
  store ptr %0, ptr %1, align 8
  br label %nullcoal_resume

nullcoal_resume:                                  ; preds = %nullcoal_null, %nullcoal_notnull
  %3 = load ptr, ptr %1, align 8
  %name1 = alloca ptr, align 8
  store ptr %3, ptr %name1, align 8
  %4 = call ptr @_tryGetName_charPtr_bool_(i1 false)
  %5 = alloca ptr, align 8
  %6 = icmp ne ptr %4, null
  br i1 %6, label %nullcoal_notnull2, label %nullcoal_null1

nullcoal_null1:                                   ; preds = %nullcoal_resume
  store ptr @95, ptr %5, align 8
  br label %nullcoal_resume3

nullcoal_notnull2:                                ; preds = %nullcoal_resume
  store ptr %4, ptr %5, align 8
  br label %nullcoal_resume3

nullcoal_resume3:                                 ; preds = %nullcoal_null1, %nullcoal_notnull2
  %7 = load ptr, ptr %5, align 8
  %name2 = alloca ptr, align 8
  store ptr %7, ptr %name2, align 8
  %8 = load ptr, ptr %name1, align 8
  %9 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @96, ptr %8, ptr @94)
  %10 = load i1, ptr %result, align 1
  %11 = and i1 %10, %9
  %tobool = icmp ne i1 %11, false
  store i1 %tobool, ptr %result, align 1
  %12 = load ptr, ptr %name2, align 8
  %13 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @97, ptr %12, ptr @95)
  %14 = load i1, ptr %result, align 1
  %15 = and i1 %14, %13
  %tobool4 = icmp ne i1 %15, false
  store i1 %tobool4, ptr %result, align 1
  %zero = alloca i32, align 4
  store i32 0, ptr %zero, align 4
  %five = alloca i32, align 4
  store i32 5, ptr %five, align 4
  %16 = load i32, ptr %zero, align 4
  %17 = alloca i32, align 4
  %tobool8 = icmp ne i32 %16, 0
  br i1 %tobool8, label %nullcoal_notnull6, label %nullcoal_null5

nullcoal_null5:                                   ; preds = %nullcoal_resume3
  store i32 42, ptr %17, align 4
  br label %nullcoal_resume7

nullcoal_notnull6:                                ; preds = %nullcoal_resume3
  store i32 %16, ptr %17, align 4
  br label %nullcoal_resume7

nullcoal_resume7:                                 ; preds = %nullcoal_null5, %nullcoal_notnull6
  %18 = load i32, ptr %17, align 4
  %19 = call i32 @_Test_int_charPtrintint_(ptr @98, i32 %18, i32 42)
  %20 = load i1, ptr %result, align 1
  %21 = zext i1 %20 to i32
  %22 = and i32 %21, %19
  %tobool9 = icmp ne i32 %22, 0
  store i1 %tobool9, ptr %result, align 1
  %23 = load i32, ptr %five, align 4
  %24 = alloca i32, align 4
  %tobool13 = icmp ne i32 %23, 0
  br i1 %tobool13, label %nullcoal_notnull11, label %nullcoal_null10

nullcoal_null10:                                  ; preds = %nullcoal_resume7
  store i32 42, ptr %24, align 4
  br label %nullcoal_resume12

nullcoal_notnull11:                               ; preds = %nullcoal_resume7
  store i32 %23, ptr %24, align 4
  br label %nullcoal_resume12

nullcoal_resume12:                                ; preds = %nullcoal_null10, %nullcoal_notnull11
  %25 = load i32, ptr %24, align 4
  %26 = call i32 @_Test_int_charPtrintint_(ptr @99, i32 %25, i32 5)
  %27 = load i1, ptr %result, align 1
  %28 = zext i1 %27 to i32
  %29 = and i32 %28, %26
  %tobool14 = icmp ne i32 %29, 0
  store i1 %tobool14, ptr %result, align 1
  %30 = load i1, ptr %result, align 1
  ret i1 %30
}

define i1 @_testDefault_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %di = alloca i32, align 4
  store i32 0, ptr %di, align 4
  %db = alloca i1, align 1
  store i1 false, ptr %db, align 1
  %di32 = alloca i32, align 4
  store i32 0, ptr %di32, align 4
  %di64 = alloca i64, align 8
  store i64 0, ptr %di64, align 4
  %0 = load i32, ptr %di, align 4
  %1 = call i32 @_Test_int_charPtrintint_(ptr @100, i32 %0, i32 0)
  %2 = load i1, ptr %result, align 1
  %3 = zext i1 %2 to i32
  %4 = and i32 %3, %1
  %tobool = icmp ne i32 %4, 0
  store i1 %tobool, ptr %result, align 1
  %5 = load i1, ptr %db, align 1
  %6 = zext i1 %5 to i32
  %7 = call i32 @_Test_int_charPtrintint_(ptr @101, i32 %6, i32 0)
  %8 = load i1, ptr %result, align 1
  %9 = zext i1 %8 to i32
  %10 = and i32 %9, %7
  %tobool1 = icmp ne i32 %10, 0
  store i1 %tobool1, ptr %result, align 1
  %11 = load i32, ptr %di32, align 4
  %12 = call i32 @_Test_int_charPtrintint_(ptr @102, i32 %11, i32 0)
  %13 = load i1, ptr %result, align 1
  %14 = zext i1 %13 to i32
  %15 = and i32 %14, %12
  %tobool2 = icmp ne i32 %15, 0
  store i1 %tobool2, ptr %result, align 1
  %16 = load i64, ptr %di64, align 4
  %17 = call i32 @_Test_int_charPtri64i64_(ptr @103, i64 %16, i64 0)
  %18 = load i1, ptr %result, align 1
  %19 = zext i1 %18 to i32
  %20 = and i32 %19, %17
  %tobool3 = icmp ne i32 %20, 0
  store i1 %tobool3, ptr %result, align 1
  %21 = call %MyStruct @_MyStruct_MyStruct__()
  %ds = alloca %MyStruct, align 8
  store %MyStruct %21, ptr %ds, align 4
  %22 = getelementptr inbounds %MyStruct, ptr %ds, i32 0, i32 0
  %23 = load i32, ptr %22, align 4
  %24 = call i32 @_Test_int_charPtrintint_(ptr @104, i32 %23, i32 1)
  %25 = load i1, ptr %result, align 1
  %26 = zext i1 %25 to i32
  %27 = and i32 %26, %24
  %tobool4 = icmp ne i32 %27, 0
  store i1 %tobool4, ptr %result, align 1
  %28 = getelementptr inbounds %MyStruct, ptr %ds, i32 0, i32 1
  %29 = load i32, ptr %28, align 4
  %30 = call i32 @_Test_int_charPtrintint_(ptr @105, i32 %29, i32 2)
  %31 = load i1, ptr %result, align 1
  %32 = zext i1 %31 to i32
  %33 = and i32 %32, %30
  %tobool5 = icmp ne i32 %33, 0
  store i1 %tobool5, ptr %result, align 1
  %34 = getelementptr inbounds %MyStruct, ptr %ds, i32 0, i32 2
  %35 = load i32, ptr %34, align 4
  %36 = call i32 @_Test_int_charPtrintint_(ptr @106, i32 %35, i32 3)
  %37 = load i1, ptr %result, align 1
  %38 = zext i1 %37 to i32
  %39 = and i32 %38, %36
  %tobool6 = icmp ne i32 %39, 0
  store i1 %tobool6, ptr %result, align 1
  %40 = load i1, ptr %result, align 1
  ret i1 %40
}

define %StringData @_StringData_StringData__() {
entry:
  ret %StringData undef
}

define ptr @_data_i8Ptr_StringDataPtr_(ptr %StringData__) {
entry:
  %0 = getelementptr inbounds %StringData, ptr %StringData__, i32 0, i32 0
  %1 = load ptr, ptr %0, align 8
  ret ptr %1
}

define i32 @_length_i32_StringDataPtr_(ptr %StringData__) {
entry:
  %0 = getelementptr inbounds %StringData, ptr %StringData__, i32 0, i32 1
  %1 = load i32, ptr %0, align 4
  ret i32 %1
}

define void @"_~StringData_void_StringDataPtr_"(ptr %StringData__) {
entry:
  %0 = getelementptr inbounds %StringData, ptr %StringData__, i32 0, i32 0
  %1 = load ptr, ptr %0, align 8
  %2 = load ptr, ptr %0, align 8
  call void @"_operator delete_void_U8Ptr_"(ptr %2)
  ret void
}

define ptr @_MakeStringData_StringDataPtr_charPtri32_(ptr %s, i32 %n) {
entry:
  %0 = call ptr @"_operator new_U8Ptr_long_"(i64 ptrtoint (ptr getelementptr (%StringData, ptr null, i32 1) to i64))
  %1 = call %StringData @_StringData_StringData__()
  store %StringData %1, ptr %0, align 8
  %sd = alloca ptr, align 8
  store ptr %0, ptr %sd, align 8
  %2 = load ptr, ptr %sd, align 8
  %3 = getelementptr inbounds %StringData, ptr %2, i32 0, i32 1
  %4 = load i32, ptr %3, align 4
  store i32 %n, ptr %3, align 4
  %5 = load ptr, ptr %sd, align 8
  %6 = getelementptr inbounds %StringData, ptr %5, i32 0, i32 0
  %7 = load ptr, ptr %6, align 8
  %8 = add i32 %n, 1
  %9 = zext i32 %8 to i64
  %arraysz = mul i64 ptrtoint (ptr getelementptr (i8, ptr null, i32 1) to i64), %9
  %10 = call ptr @"_operator new_U8Ptr_long_"(i64 %arraysz)
  store ptr %10, ptr %6, align 8
  br label %forInit

forInit:                                          ; preds = %entry
  %i = alloca i32, align 4
  store i32 0, ptr %i, align 4
  br label %forIncrement

forCondition:                                     ; preds = %forIncrement
  %11 = load i32, ptr %i, align 4
  %12 = icmp slt i32 %11, %n
  br i1 %12, label %forInner, label %forResume

forInner:                                         ; preds = %forCondition
  %13 = load ptr, ptr %sd, align 8
  %14 = getelementptr inbounds %StringData, ptr %13, i32 0, i32 0
  %15 = load ptr, ptr %14, align 8
  %16 = load i32, ptr %i, align 4
  %17 = getelementptr i8, ptr %15, i32 %16
  %18 = load i32, ptr %i, align 4
  %19 = getelementptr i8, ptr %s, i32 %18
  %20 = load i8, ptr %19, align 1
  store i8 %20, ptr %17, align 1
  br label %forIncrement

forIncrement:                                     ; preds = %forInner, %forInit
  %21 = load i32, ptr %i, align 4
  %22 = load i32, ptr %i, align 4
  %23 = add i32 %22, 1
  store i32 %23, ptr %i, align 4
  br label %forCondition

forResume:                                        ; preds = %forCondition
  %24 = load ptr, ptr %sd, align 8
  %25 = getelementptr inbounds %StringData, ptr %24, i32 0, i32 0
  %26 = load ptr, ptr %25, align 8
  %27 = getelementptr i8, ptr %26, i32 %n
  store i8 0, ptr %27, align 1
  %28 = load ptr, ptr %sd, align 8
  ret ptr %28
}

define %__iface_fat_ptr @"_operator string_IReadOnlyStringPtr_charPtr_"(ptr %s) {
entry:
  %0 = call i32 @strlen(ptr %s)
  %1 = call ptr @_MakeStringData_StringDataPtr_charPtri32_(ptr %s, i32 %0)
  %sd = alloca ptr, align 8
  store ptr %1, ptr %sd, align 8
  %2 = load ptr, ptr %sd, align 8
  %3 = insertvalue %__iface_fat_ptr { ptr @StringData_IReadOnlyString_vtable, ptr undef }, ptr %2, 1
  %result = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %3, ptr %result, align 8
  %4 = load %__iface_fat_ptr, ptr %result, align 8
  ret %__iface_fat_ptr %4
}

define i1 @_testStringType_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %strlitval = alloca %__StrLit, align 8
  %0 = getelementptr inbounds %__StrLit, ptr %strlitval, i32 0, i32 0
  store ptr @107, ptr %0, align 8
  %1 = getelementptr inbounds %__StrLit, ptr %strlitval, i32 0, i32 1
  store i32 5, ptr %1, align 4
  %2 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %strlitval, 1
  %s = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %2, ptr %s, align 8
  %3 = getelementptr inbounds %__iface_fat_ptr, ptr %s, i32 0, i32 0
  %4 = load ptr, ptr %3, align 8
  %5 = getelementptr inbounds %__iface_fat_ptr, ptr %s, i32 0, i32 1
  %6 = load ptr, ptr %5, align 8
  %7 = getelementptr ptr, ptr %4, i32 1
  %8 = load ptr, ptr %7, align 8
  %9 = call i32 %8(ptr %6)
  %10 = call i32 @_Test_int_charPtrintint_(ptr @108, i32 %9, i32 5)
  %11 = load i1, ptr %result, align 1
  %12 = zext i1 %11 to i32
  %13 = and i32 %12, %10
  %tobool = icmp ne i32 %13, 0
  store i1 %tobool, ptr %result, align 1
  %14 = getelementptr inbounds %__iface_fat_ptr, ptr %s, i32 0, i32 0
  %15 = load ptr, ptr %14, align 8
  %16 = getelementptr inbounds %__iface_fat_ptr, ptr %s, i32 0, i32 1
  %17 = load ptr, ptr %16, align 8
  %18 = getelementptr ptr, ptr %15, i32 0
  %19 = load ptr, ptr %18, align 8
  %20 = call ptr %19(ptr %17)
  %21 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @109, ptr %20, ptr @107)
  %22 = load i1, ptr %result, align 1
  %23 = and i1 %22, %21
  %tobool1 = icmp ne i1 %23, false
  store i1 %tobool1, ptr %result, align 1
  %strlitval2 = alloca %__StrLit, align 8
  %24 = getelementptr inbounds %__StrLit, ptr %strlitval2, i32 0, i32 0
  store ptr @110, ptr %24, align 8
  %25 = getelementptr inbounds %__StrLit, ptr %strlitval2, i32 0, i32 1
  store i32 7, ptr %25, align 4
  %26 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %strlitval2, 1
  %s2 = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %26, ptr %s2, align 8
  %27 = getelementptr inbounds %__iface_fat_ptr, ptr %s2, i32 0, i32 0
  %28 = load ptr, ptr %27, align 8
  %29 = getelementptr inbounds %__iface_fat_ptr, ptr %s2, i32 0, i32 1
  %30 = load ptr, ptr %29, align 8
  %31 = getelementptr ptr, ptr %28, i32 1
  %32 = load ptr, ptr %31, align 8
  %33 = call i32 %32(ptr %30)
  %34 = call i32 @_Test_int_charPtrintint_(ptr @111, i32 %33, i32 7)
  %35 = load i1, ptr %result, align 1
  %36 = zext i1 %35 to i32
  %37 = and i32 %36, %34
  %tobool3 = icmp ne i32 %37, 0
  store i1 %tobool3, ptr %result, align 1
  %38 = getelementptr inbounds %__iface_fat_ptr, ptr %s2, i32 0, i32 0
  %39 = load ptr, ptr %38, align 8
  %40 = getelementptr inbounds %__iface_fat_ptr, ptr %s2, i32 0, i32 1
  %41 = load ptr, ptr %40, align 8
  %42 = getelementptr ptr, ptr %39, i32 0
  %43 = load ptr, ptr %42, align 8
  %44 = call ptr %43(ptr %41)
  %45 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @112, ptr %44, ptr @110)
  %46 = load i1, ptr %result, align 1
  %47 = and i1 %46, %45
  %tobool4 = icmp ne i1 %47, false
  store i1 %tobool4, ptr %result, align 1
  %48 = load i1, ptr %result, align 1
  ret i1 %48
}

define i1 @_testStringLiteral_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %strlitval = alloca %__StrLit, align 8
  %0 = getelementptr inbounds %__StrLit, ptr %strlitval, i32 0, i32 0
  store ptr @107, ptr %0, align 8
  %1 = getelementptr inbounds %__StrLit, ptr %strlitval, i32 0, i32 1
  store i32 5, ptr %1, align 4
  %2 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %strlitval, 1
  %s = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %2, ptr %s, align 8
  %3 = getelementptr inbounds %__iface_fat_ptr, ptr %s, i32 0, i32 0
  %4 = load ptr, ptr %3, align 8
  %5 = getelementptr inbounds %__iface_fat_ptr, ptr %s, i32 0, i32 1
  %6 = load ptr, ptr %5, align 8
  %7 = getelementptr ptr, ptr %4, i32 1
  %8 = load ptr, ptr %7, align 8
  %9 = call i32 %8(ptr %6)
  %10 = call i32 @_Test_int_charPtrintint_(ptr @113, i32 %9, i32 5)
  %11 = load i1, ptr %result, align 1
  %12 = zext i1 %11 to i32
  %13 = and i32 %12, %10
  %tobool = icmp ne i32 %13, 0
  store i1 %tobool, ptr %result, align 1
  %14 = getelementptr inbounds %__iface_fat_ptr, ptr %s, i32 0, i32 0
  %15 = load ptr, ptr %14, align 8
  %16 = getelementptr inbounds %__iface_fat_ptr, ptr %s, i32 0, i32 1
  %17 = load ptr, ptr %16, align 8
  %18 = getelementptr ptr, ptr %15, i32 0
  %19 = load ptr, ptr %18, align 8
  %20 = call ptr %19(ptr %17)
  %21 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @114, ptr %20, ptr @107)
  %22 = load i1, ptr %result, align 1
  %23 = and i1 %22, %21
  %tobool1 = icmp ne i1 %23, false
  store i1 %tobool1, ptr %result, align 1
  %strlitval2 = alloca %__StrLit, align 8
  %24 = getelementptr inbounds %__StrLit, ptr %strlitval2, i32 0, i32 0
  store ptr @115, ptr %24, align 8
  %25 = getelementptr inbounds %__StrLit, ptr %strlitval2, i32 0, i32 1
  store i32 14, ptr %25, align 4
  %26 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %strlitval2, 1
  %s2 = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %26, ptr %s2, align 8
  %27 = getelementptr inbounds %__iface_fat_ptr, ptr %s2, i32 0, i32 0
  %28 = load ptr, ptr %27, align 8
  %29 = getelementptr inbounds %__iface_fat_ptr, ptr %s2, i32 0, i32 1
  %30 = load ptr, ptr %29, align 8
  %31 = getelementptr ptr, ptr %28, i32 1
  %32 = load ptr, ptr %31, align 8
  %33 = call i32 %32(ptr %30)
  %34 = call i32 @_Test_int_charPtrintint_(ptr @116, i32 %33, i32 14)
  %35 = load i1, ptr %result, align 1
  %36 = zext i1 %35 to i32
  %37 = and i32 %36, %34
  %tobool3 = icmp ne i32 %37, 0
  store i1 %tobool3, ptr %result, align 1
  %38 = getelementptr inbounds %__iface_fat_ptr, ptr %s2, i32 0, i32 0
  %39 = load ptr, ptr %38, align 8
  %40 = getelementptr inbounds %__iface_fat_ptr, ptr %s2, i32 0, i32 1
  %41 = load ptr, ptr %40, align 8
  %42 = getelementptr ptr, ptr %39, i32 0
  %43 = load ptr, ptr %42, align 8
  %44 = call ptr %43(ptr %41)
  %45 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @117, ptr %44, ptr @115)
  %46 = load i1, ptr %result, align 1
  %47 = and i1 %46, %45
  %tobool4 = icmp ne i1 %47, false
  store i1 %tobool4, ptr %result, align 1
  %strlitval5 = alloca %__StrLit, align 8
  %48 = getelementptr inbounds %__StrLit, ptr %strlitval5, i32 0, i32 0
  store ptr @118, ptr %48, align 8
  %49 = getelementptr inbounds %__StrLit, ptr %strlitval5, i32 0, i32 1
  store i32 0, ptr %49, align 4
  %50 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %strlitval5, 1
  %s3 = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %50, ptr %s3, align 8
  %51 = getelementptr inbounds %__iface_fat_ptr, ptr %s3, i32 0, i32 0
  %52 = load ptr, ptr %51, align 8
  %53 = getelementptr inbounds %__iface_fat_ptr, ptr %s3, i32 0, i32 1
  %54 = load ptr, ptr %53, align 8
  %55 = getelementptr ptr, ptr %52, i32 1
  %56 = load ptr, ptr %55, align 8
  %57 = call i32 %56(ptr %54)
  %58 = call i32 @_Test_int_charPtrintint_(ptr @119, i32 %57, i32 0)
  %59 = load i1, ptr %result, align 1
  %60 = zext i1 %59 to i32
  %61 = and i32 %60, %58
  %tobool6 = icmp ne i32 %61, 0
  store i1 %tobool6, ptr %result, align 1
  %62 = getelementptr inbounds %__iface_fat_ptr, ptr %s3, i32 0, i32 0
  %63 = load ptr, ptr %62, align 8
  %64 = getelementptr inbounds %__iface_fat_ptr, ptr %s3, i32 0, i32 1
  %65 = load ptr, ptr %64, align 8
  %66 = getelementptr ptr, ptr %63, i32 0
  %67 = load ptr, ptr %66, align 8
  %68 = call ptr %67(ptr %65)
  %69 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @120, ptr %68, ptr @118)
  %70 = load i1, ptr %result, align 1
  %71 = and i1 %70, %69
  %tobool7 = icmp ne i1 %71, false
  store i1 %tobool7, ptr %result, align 1
  %strlitval8 = alloca %__StrLit, align 8
  %72 = getelementptr inbounds %__StrLit, ptr %strlitval8, i32 0, i32 0
  store ptr @121, ptr %72, align 8
  %73 = getelementptr inbounds %__StrLit, ptr %strlitval8, i32 0, i32 1
  store i32 11, ptr %73, align 4
  %74 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %strlitval8, 1
  %s4 = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %74, ptr %s4, align 8
  %75 = getelementptr inbounds %__iface_fat_ptr, ptr %s4, i32 0, i32 0
  %76 = load ptr, ptr %75, align 8
  %77 = getelementptr inbounds %__iface_fat_ptr, ptr %s4, i32 0, i32 1
  %78 = load ptr, ptr %77, align 8
  %79 = getelementptr ptr, ptr %76, i32 1
  %80 = load ptr, ptr %79, align 8
  %81 = call i32 %80(ptr %78)
  %82 = call i32 @_Test_int_charPtrintint_(ptr @122, i32 %81, i32 11)
  %83 = load i1, ptr %result, align 1
  %84 = zext i1 %83 to i32
  %85 = and i32 %84, %82
  %tobool9 = icmp ne i32 %85, 0
  store i1 %tobool9, ptr %result, align 1
  %strlitval10 = alloca %__StrLit, align 8
  %86 = getelementptr inbounds %__StrLit, ptr %strlitval10, i32 0, i32 0
  store ptr @123, ptr %86, align 8
  %87 = getelementptr inbounds %__StrLit, ptr %strlitval10, i32 0, i32 1
  store i32 5, ptr %87, align 4
  %88 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %strlitval10, 1
  %sa = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %88, ptr %sa, align 8
  %strlitval11 = alloca %__StrLit, align 8
  %89 = getelementptr inbounds %__StrLit, ptr %strlitval11, i32 0, i32 0
  store ptr @124, ptr %89, align 8
  %90 = getelementptr inbounds %__StrLit, ptr %strlitval11, i32 0, i32 1
  store i32 5, ptr %90, align 4
  %91 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %strlitval11, 1
  %sb = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %91, ptr %sb, align 8
  %92 = getelementptr inbounds %__iface_fat_ptr, ptr %sa, i32 0, i32 0
  %93 = load ptr, ptr %92, align 8
  %94 = getelementptr inbounds %__iface_fat_ptr, ptr %sa, i32 0, i32 1
  %95 = load ptr, ptr %94, align 8
  %96 = getelementptr ptr, ptr %93, i32 1
  %97 = load ptr, ptr %96, align 8
  %98 = call i32 %97(ptr %95)
  %99 = call i32 @_Test_int_charPtrintint_(ptr @125, i32 %98, i32 5)
  %100 = load i1, ptr %result, align 1
  %101 = zext i1 %100 to i32
  %102 = and i32 %101, %99
  %tobool12 = icmp ne i32 %102, 0
  store i1 %tobool12, ptr %result, align 1
  %103 = getelementptr inbounds %__iface_fat_ptr, ptr %sb, i32 0, i32 0
  %104 = load ptr, ptr %103, align 8
  %105 = getelementptr inbounds %__iface_fat_ptr, ptr %sb, i32 0, i32 1
  %106 = load ptr, ptr %105, align 8
  %107 = getelementptr ptr, ptr %104, i32 1
  %108 = load ptr, ptr %107, align 8
  %109 = call i32 %108(ptr %106)
  %110 = call i32 @_Test_int_charPtrintint_(ptr @126, i32 %109, i32 5)
  %111 = load i1, ptr %result, align 1
  %112 = zext i1 %111 to i32
  %113 = and i32 %112, %110
  %tobool13 = icmp ne i32 %113, 0
  store i1 %tobool13, ptr %result, align 1
  %114 = getelementptr inbounds %__iface_fat_ptr, ptr %sa, i32 0, i32 0
  %115 = load ptr, ptr %114, align 8
  %116 = getelementptr inbounds %__iface_fat_ptr, ptr %sa, i32 0, i32 1
  %117 = load ptr, ptr %116, align 8
  %118 = getelementptr ptr, ptr %115, i32 0
  %119 = load ptr, ptr %118, align 8
  %120 = call ptr %119(ptr %117)
  %121 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @127, ptr %120, ptr @123)
  %122 = load i1, ptr %result, align 1
  %123 = and i1 %122, %121
  %tobool14 = icmp ne i1 %123, false
  store i1 %tobool14, ptr %result, align 1
  %124 = getelementptr inbounds %__iface_fat_ptr, ptr %sb, i32 0, i32 0
  %125 = load ptr, ptr %124, align 8
  %126 = getelementptr inbounds %__iface_fat_ptr, ptr %sb, i32 0, i32 1
  %127 = load ptr, ptr %126, align 8
  %128 = getelementptr ptr, ptr %125, i32 0
  %129 = load ptr, ptr %128, align 8
  %130 = call ptr %129(ptr %127)
  %131 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @128, ptr %130, ptr @124)
  %132 = load i1, ptr %result, align 1
  %133 = and i1 %132, %131
  %tobool15 = icmp ne i1 %133, false
  store i1 %tobool15, ptr %result, align 1
  %134 = load i1, ptr %result, align 1
  ret i1 %134
}

define %Node @_Node_Node__() {
entry:
  ret %Node zeroinitializer
}

define i32 @_testNewDelete_int__() {
entry:
  %0 = call ptr @"_operator new_U8Ptr_long_"(i64 ptrtoint (ptr getelementptr (%Node, ptr null, i32 1) to i64))
  %1 = call %Node @_Node_Node__()
  store %Node %1, ptr %0, align 4
  %n = alloca ptr, align 8
  store ptr %0, ptr %n, align 8
  %2 = load ptr, ptr %n, align 8
  call void @"_operator delete_void_U8Ptr_"(ptr %2)
  ret i32 42
}

define i32 @_testNewArray_int__() {
entry:
  %0 = call ptr @"_operator new_U8Ptr_long_"(i64 mul (i64 ptrtoint (ptr getelementptr (i32, ptr null, i32 1) to i64), i64 5))
  %arr = alloca ptr, align 8
  store ptr %0, ptr %arr, align 8
  br label %forInit

forInit:                                          ; preds = %entry
  %i = alloca i32, align 4
  store i32 0, ptr %i, align 4
  br label %forIncrement

forCondition:                                     ; preds = %forIncrement
  %1 = load i32, ptr %i, align 4
  %2 = icmp slt i32 %1, 5
  br i1 %2, label %forInner, label %forResume

forInner:                                         ; preds = %forCondition
  %3 = load i32, ptr %i, align 4
  %4 = load ptr, ptr %arr, align 8
  %5 = getelementptr i32, ptr %4, i32 %3
  %6 = load i32, ptr %i, align 4
  store i32 %6, ptr %5, align 4
  br label %forIncrement

forIncrement:                                     ; preds = %forInner, %forInit
  %7 = load i32, ptr %i, align 4
  %8 = load i32, ptr %i, align 4
  %9 = add i32 %8, 1
  store i32 %9, ptr %i, align 4
  br label %forCondition

forResume:                                        ; preds = %forCondition
  br label %forInit1

forInit1:                                         ; preds = %forResume
  %i6 = alloca i32, align 4
  store i32 0, ptr %i6, align 4
  br label %forIncrement4

forCondition2:                                    ; preds = %forIncrement4
  %10 = load i32, ptr %i6, align 4
  %11 = icmp slt i32 %10, 5
  br i1 %11, label %forInner3, label %forResume5

forInner3:                                        ; preds = %forCondition2
  br label %ifCondition

forIncrement4:                                    ; preds = %ifResume, %forInit1
  %12 = load i32, ptr %i6, align 4
  %13 = load i32, ptr %i6, align 4
  %14 = add i32 %13, 1
  store i32 %14, ptr %i6, align 4
  br label %forCondition2

forResume5:                                       ; preds = %forCondition2
  %15 = load ptr, ptr %arr, align 8
  call void @"_operator delete_void_U8Ptr_"(ptr %15)
  ret i32 99

ifCondition:                                      ; preds = %forInner3
  %16 = load i32, ptr %i6, align 4
  %17 = load ptr, ptr %arr, align 8
  %18 = getelementptr i32, ptr %17, i32 %16
  %19 = load i32, ptr %18, align 4
  %20 = load i32, ptr %i6, align 4
  %21 = icmp ne i32 %19, %20
  br i1 %21, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  %22 = load i32, ptr %i6, align 4
  %23 = load i32, ptr %i6, align 4
  %24 = load i32, ptr %i6, align 4
  %25 = load ptr, ptr %arr, align 8
  %26 = getelementptr i32, ptr %25, i32 %24
  %27 = load i32, ptr %26, align 4
  call void (ptr, ...) @printf(ptr @129, i32 %22, i32 %23, i32 %27)
  %28 = load ptr, ptr %arr, align 8
  call void @"_operator delete_void_U8Ptr_"(ptr %28)
  ret i32 1

ifResume:                                         ; preds = %ifCondition
  br label %forIncrement4
}

define %Counter @_Counter_Counter__() {
entry:
  ret %Counter zeroinitializer
}

define i32 @_testNewWithConstructor_int__() {
entry:
  %0 = call ptr @"_operator new_U8Ptr_long_"(i64 ptrtoint (ptr getelementptr (%Counter, ptr null, i32 1) to i64))
  %1 = call %Counter @_Counter_Counter__()
  store %Counter %1, ptr %0, align 4
  %c = alloca ptr, align 8
  store ptr %0, ptr %c, align 8
  %2 = load ptr, ptr %c, align 8
  call void @"_operator delete_void_U8Ptr_"(ptr %2)
  ret i32 123
}

define %Point @_Point_Point__() {
entry:
  ret %Point undef
}

define %__iface_fat_ptr @_ToString_IReadOnlyStringPtr_PointPtr_(ptr %Point__) {
entry:
  %0 = getelementptr inbounds %Point, ptr %Point__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  %2 = inttoptr i32 %1 to ptr
  %3 = call %__iface_fat_ptr @"_operator string_IReadOnlyStringPtr_charPtr_"(ptr %2)
  %sx = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %3, ptr %sx, align 8
  %4 = getelementptr inbounds %Point, ptr %Point__, i32 0, i32 1
  %5 = load i32, ptr %4, align 4
  %6 = inttoptr i32 %5 to ptr
  %7 = call %__iface_fat_ptr @"_operator string_IReadOnlyStringPtr_charPtr_"(ptr %6)
  %sy = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %7, ptr %sy, align 8
  %fatptr = alloca %__iface_fat_ptr, align 8
  %8 = load %__iface_fat_ptr, ptr %sx, align 8
  store %__iface_fat_ptr %8, ptr %fatptr, align 8
  %9 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr, i32 0, i32 0
  %10 = load ptr, ptr %9, align 8
  %11 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr, i32 0, i32 1
  %12 = load ptr, ptr %11, align 8
  %13 = getelementptr ptr, ptr %10, i32 0
  %14 = load ptr, ptr %13, align 8
  %15 = call ptr %14(ptr %12)
  %16 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr, i32 0, i32 0
  %17 = load ptr, ptr %16, align 8
  %18 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr, i32 0, i32 1
  %19 = load ptr, ptr %18, align 8
  %20 = getelementptr ptr, ptr %17, i32 1
  %21 = load ptr, ptr %20, align 8
  %22 = call i32 %21(ptr %19)
  %fatptr1 = alloca %__iface_fat_ptr, align 8
  %23 = load %__iface_fat_ptr, ptr %sy, align 8
  store %__iface_fat_ptr %23, ptr %fatptr1, align 8
  %24 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr1, i32 0, i32 0
  %25 = load ptr, ptr %24, align 8
  %26 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr1, i32 0, i32 1
  %27 = load ptr, ptr %26, align 8
  %28 = getelementptr ptr, ptr %25, i32 0
  %29 = load ptr, ptr %28, align 8
  %30 = call ptr %29(ptr %27)
  %31 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr1, i32 0, i32 0
  %32 = load ptr, ptr %31, align 8
  %33 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr1, i32 0, i32 1
  %34 = load ptr, ptr %33, align 8
  %35 = getelementptr ptr, ptr %32, i32 1
  %36 = load ptr, ptr %35, align 8
  %37 = call i32 %36(ptr %34)
  %fmtptrs = alloca [5 x ptr], align 8
  %fmtlens = alloca [5 x i32], align 4
  %38 = getelementptr inbounds [5 x ptr], ptr %fmtptrs, i32 0, i32 0
  store ptr @fmtlit, ptr %38, align 8
  %39 = getelementptr inbounds [5 x i32], ptr %fmtlens, i32 0, i32 0
  store i32 1, ptr %39, align 4
  %40 = getelementptr inbounds [5 x ptr], ptr %fmtptrs, i32 0, i32 1
  store ptr %15, ptr %40, align 8
  %41 = getelementptr inbounds [5 x i32], ptr %fmtlens, i32 0, i32 1
  store i32 %22, ptr %41, align 4
  %42 = getelementptr inbounds [5 x ptr], ptr %fmtptrs, i32 0, i32 2
  store ptr @fmtlit.6, ptr %42, align 8
  %43 = getelementptr inbounds [5 x i32], ptr %fmtlens, i32 0, i32 2
  store i32 2, ptr %43, align 4
  %44 = getelementptr inbounds [5 x ptr], ptr %fmtptrs, i32 0, i32 3
  store ptr %30, ptr %44, align 8
  %45 = getelementptr inbounds [5 x i32], ptr %fmtlens, i32 0, i32 3
  store i32 %37, ptr %45, align 4
  %46 = getelementptr inbounds [5 x ptr], ptr %fmtptrs, i32 0, i32 4
  store ptr @fmtlit.7, ptr %46, align 8
  %47 = getelementptr inbounds [5 x i32], ptr %fmtlens, i32 0, i32 4
  store i32 1, ptr %47, align 4
  %48 = getelementptr inbounds [5 x ptr], ptr %fmtptrs, i32 0, i32 0
  %49 = getelementptr inbounds [5 x i32], ptr %fmtlens, i32 0, i32 0
  %50 = call %__iface_fat_ptr @__strconcat(ptr %48, ptr %49, i32 5)
  ret %__iface_fat_ptr %50
}

define i1 @_testFormatString_bool__() {
entry:
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %strlitval = alloca %__StrLit, align 8
  %0 = getelementptr inbounds %__StrLit, ptr %strlitval, i32 0, i32 0
  store ptr @130, ptr %0, align 8
  %1 = getelementptr inbounds %__StrLit, ptr %strlitval, i32 0, i32 1
  store i32 5, ptr %1, align 4
  %2 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %strlitval, 1
  %name = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %2, ptr %name, align 8
  %fatptr = alloca %__iface_fat_ptr, align 8
  %3 = load %__iface_fat_ptr, ptr %name, align 8
  store %__iface_fat_ptr %3, ptr %fatptr, align 8
  %4 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr, i32 0, i32 0
  %5 = load ptr, ptr %4, align 8
  %6 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr, i32 0, i32 1
  %7 = load ptr, ptr %6, align 8
  %8 = getelementptr ptr, ptr %5, i32 0
  %9 = load ptr, ptr %8, align 8
  %10 = call ptr %9(ptr %7)
  %11 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr, i32 0, i32 0
  %12 = load ptr, ptr %11, align 8
  %13 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr, i32 0, i32 1
  %14 = load ptr, ptr %13, align 8
  %15 = getelementptr ptr, ptr %12, i32 1
  %16 = load ptr, ptr %15, align 8
  %17 = call i32 %16(ptr %14)
  %fmtptrs = alloca [3 x ptr], align 8
  %fmtlens = alloca [3 x i32], align 4
  %18 = getelementptr inbounds [3 x ptr], ptr %fmtptrs, i32 0, i32 0
  store ptr @fmtlit.8, ptr %18, align 8
  %19 = getelementptr inbounds [3 x i32], ptr %fmtlens, i32 0, i32 0
  store i32 6, ptr %19, align 4
  %20 = getelementptr inbounds [3 x ptr], ptr %fmtptrs, i32 0, i32 1
  store ptr %10, ptr %20, align 8
  %21 = getelementptr inbounds [3 x i32], ptr %fmtlens, i32 0, i32 1
  store i32 %17, ptr %21, align 4
  %22 = getelementptr inbounds [3 x ptr], ptr %fmtptrs, i32 0, i32 2
  store ptr @fmtlit.9, ptr %22, align 8
  %23 = getelementptr inbounds [3 x i32], ptr %fmtlens, i32 0, i32 2
  store i32 1, ptr %23, align 4
  %24 = getelementptr inbounds [3 x ptr], ptr %fmtptrs, i32 0, i32 0
  %25 = getelementptr inbounds [3 x i32], ptr %fmtlens, i32 0, i32 0
  %26 = call %__iface_fat_ptr @__strconcat(ptr %24, ptr %25, i32 3)
  %greeting = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %26, ptr %greeting, align 8
  %27 = getelementptr inbounds %__iface_fat_ptr, ptr %greeting, i32 0, i32 0
  %28 = load ptr, ptr %27, align 8
  %29 = getelementptr inbounds %__iface_fat_ptr, ptr %greeting, i32 0, i32 1
  %30 = load ptr, ptr %29, align 8
  %31 = getelementptr ptr, ptr %28, i32 0
  %32 = load ptr, ptr %31, align 8
  %33 = call ptr %32(ptr %30)
  %34 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @131, ptr %33, ptr @132)
  %35 = load i1, ptr %result, align 1
  %36 = and i1 %35, %34
  %tobool = icmp ne i1 %36, false
  store i1 %tobool, ptr %result, align 1
  %count = alloca i32, align 4
  store i32 42, ptr %count, align 4
  %37 = load i32, ptr %count, align 4
  %38 = inttoptr i32 %37 to ptr
  %39 = call %__iface_fat_ptr @"_operator string_IReadOnlyStringPtr_charPtr_"(ptr %38)
  %sc = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %39, ptr %sc, align 8
  %fatptr1 = alloca %__iface_fat_ptr, align 8
  %40 = load %__iface_fat_ptr, ptr %sc, align 8
  store %__iface_fat_ptr %40, ptr %fatptr1, align 8
  %41 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr1, i32 0, i32 0
  %42 = load ptr, ptr %41, align 8
  %43 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr1, i32 0, i32 1
  %44 = load ptr, ptr %43, align 8
  %45 = getelementptr ptr, ptr %42, i32 0
  %46 = load ptr, ptr %45, align 8
  %47 = call ptr %46(ptr %44)
  %48 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr1, i32 0, i32 0
  %49 = load ptr, ptr %48, align 8
  %50 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr1, i32 0, i32 1
  %51 = load ptr, ptr %50, align 8
  %52 = getelementptr ptr, ptr %49, i32 1
  %53 = load ptr, ptr %52, align 8
  %54 = call i32 %53(ptr %51)
  %fmtptrs2 = alloca [3 x ptr], align 8
  %fmtlens3 = alloca [3 x i32], align 4
  %55 = getelementptr inbounds [3 x ptr], ptr %fmtptrs2, i32 0, i32 0
  store ptr @fmtlit.10, ptr %55, align 8
  %56 = getelementptr inbounds [3 x i32], ptr %fmtlens3, i32 0, i32 0
  store i32 9, ptr %56, align 4
  %57 = getelementptr inbounds [3 x ptr], ptr %fmtptrs2, i32 0, i32 1
  store ptr %47, ptr %57, align 8
  %58 = getelementptr inbounds [3 x i32], ptr %fmtlens3, i32 0, i32 1
  store i32 %54, ptr %58, align 4
  %59 = getelementptr inbounds [3 x ptr], ptr %fmtptrs2, i32 0, i32 2
  store ptr @fmtlit.11, ptr %59, align 8
  %60 = getelementptr inbounds [3 x i32], ptr %fmtlens3, i32 0, i32 2
  store i32 1, ptr %60, align 4
  %61 = getelementptr inbounds [3 x ptr], ptr %fmtptrs2, i32 0, i32 0
  %62 = getelementptr inbounds [3 x i32], ptr %fmtlens3, i32 0, i32 0
  %63 = call %__iface_fat_ptr @__strconcat(ptr %61, ptr %62, i32 3)
  %msg = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %63, ptr %msg, align 8
  %64 = getelementptr inbounds %__iface_fat_ptr, ptr %msg, i32 0, i32 0
  %65 = load ptr, ptr %64, align 8
  %66 = getelementptr inbounds %__iface_fat_ptr, ptr %msg, i32 0, i32 1
  %67 = load ptr, ptr %66, align 8
  %68 = getelementptr ptr, ptr %65, i32 0
  %69 = load ptr, ptr %68, align 8
  %70 = call ptr %69(ptr %67)
  %71 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @133, ptr %70, ptr @134)
  %72 = load i1, ptr %result, align 1
  %73 = and i1 %72, %71
  %tobool4 = icmp ne i1 %73, false
  store i1 %tobool4, ptr %result, align 1
  %strlitval5 = alloca %__StrLit, align 8
  %74 = getelementptr inbounds %__StrLit, ptr %strlitval5, i32 0, i32 0
  store ptr @135, ptr %74, align 8
  %75 = getelementptr inbounds %__StrLit, ptr %strlitval5, i32 0, i32 1
  store i32 3, ptr %75, align 4
  %76 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %strlitval5, 1
  %a = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %76, ptr %a, align 8
  %strlitval6 = alloca %__StrLit, align 8
  %77 = getelementptr inbounds %__StrLit, ptr %strlitval6, i32 0, i32 0
  store ptr @136, ptr %77, align 8
  %78 = getelementptr inbounds %__StrLit, ptr %strlitval6, i32 0, i32 1
  store i32 3, ptr %78, align 4
  %79 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %strlitval6, 1
  %b = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %79, ptr %b, align 8
  %fatptr7 = alloca %__iface_fat_ptr, align 8
  %80 = load %__iface_fat_ptr, ptr %a, align 8
  store %__iface_fat_ptr %80, ptr %fatptr7, align 8
  %81 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr7, i32 0, i32 0
  %82 = load ptr, ptr %81, align 8
  %83 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr7, i32 0, i32 1
  %84 = load ptr, ptr %83, align 8
  %85 = getelementptr ptr, ptr %82, i32 0
  %86 = load ptr, ptr %85, align 8
  %87 = call ptr %86(ptr %84)
  %88 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr7, i32 0, i32 0
  %89 = load ptr, ptr %88, align 8
  %90 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr7, i32 0, i32 1
  %91 = load ptr, ptr %90, align 8
  %92 = getelementptr ptr, ptr %89, i32 1
  %93 = load ptr, ptr %92, align 8
  %94 = call i32 %93(ptr %91)
  %fatptr8 = alloca %__iface_fat_ptr, align 8
  %95 = load %__iface_fat_ptr, ptr %b, align 8
  store %__iface_fat_ptr %95, ptr %fatptr8, align 8
  %96 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr8, i32 0, i32 0
  %97 = load ptr, ptr %96, align 8
  %98 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr8, i32 0, i32 1
  %99 = load ptr, ptr %98, align 8
  %100 = getelementptr ptr, ptr %97, i32 0
  %101 = load ptr, ptr %100, align 8
  %102 = call ptr %101(ptr %99)
  %103 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr8, i32 0, i32 0
  %104 = load ptr, ptr %103, align 8
  %105 = getelementptr inbounds %__iface_fat_ptr, ptr %fatptr8, i32 0, i32 1
  %106 = load ptr, ptr %105, align 8
  %107 = getelementptr ptr, ptr %104, i32 1
  %108 = load ptr, ptr %107, align 8
  %109 = call i32 %108(ptr %106)
  %fmtptrs9 = alloca [3 x ptr], align 8
  %fmtlens10 = alloca [3 x i32], align 4
  %110 = getelementptr inbounds [3 x ptr], ptr %fmtptrs9, i32 0, i32 0
  store ptr %87, ptr %110, align 8
  %111 = getelementptr inbounds [3 x i32], ptr %fmtlens10, i32 0, i32 0
  store i32 %94, ptr %111, align 4
  %112 = getelementptr inbounds [3 x ptr], ptr %fmtptrs9, i32 0, i32 1
  store ptr @fmtlit.12, ptr %112, align 8
  %113 = getelementptr inbounds [3 x i32], ptr %fmtlens10, i32 0, i32 1
  store i32 5, ptr %113, align 4
  %114 = getelementptr inbounds [3 x ptr], ptr %fmtptrs9, i32 0, i32 2
  store ptr %102, ptr %114, align 8
  %115 = getelementptr inbounds [3 x i32], ptr %fmtlens10, i32 0, i32 2
  store i32 %109, ptr %115, align 4
  %116 = getelementptr inbounds [3 x ptr], ptr %fmtptrs9, i32 0, i32 0
  %117 = getelementptr inbounds [3 x i32], ptr %fmtlens10, i32 0, i32 0
  %118 = call %__iface_fat_ptr @__strconcat(ptr %116, ptr %117, i32 3)
  %combined = alloca %__iface_fat_ptr, align 8
  store %__iface_fat_ptr %118, ptr %combined, align 8
  %119 = getelementptr inbounds %__iface_fat_ptr, ptr %combined, i32 0, i32 0
  %120 = load ptr, ptr %119, align 8
  %121 = getelementptr inbounds %__iface_fat_ptr, ptr %combined, i32 0, i32 1
  %122 = load ptr, ptr %121, align 8
  %123 = getelementptr ptr, ptr %120, i32 0
  %124 = load ptr, ptr %123, align 8
  %125 = call ptr %124(ptr %122)
  %126 = call i1 @_TestStr_bool_charPtrcharPtrcharPtr_(ptr @137, ptr %125, ptr @138)
  %127 = load i1, ptr %result, align 1
  %128 = and i1 %127, %126
  %tobool11 = icmp ne i1 %128, false
  store i1 %tobool11, ptr %result, align 1
  %129 = load i1, ptr %result, align 1
  ret i1 %129
}

define i32 @main() {
entry:
  %0 = call %MyStruct @_MyStruct_MyStruct__()
  %my = alloca %MyStruct, align 8
  store %MyStruct %0, ptr %my, align 4
  %1 = call %MyStruct1 @_MyStruct1_MyStruct1__()
  %struct1 = alloca %MyStruct1, align 8
  store %MyStruct1 %1, ptr %struct1, align 4
  %2 = call %MyStruct2 @_MyStruct2_MyStruct2__()
  %struct2 = alloca %MyStruct2, align 8
  store %MyStruct2 %2, ptr %struct2, align 4
  %result = alloca i1, align 1
  store i1 true, ptr %result, align 1
  %3 = call i32 @_testArray_int__()
  %4 = call i32 @_Test_int_charPtrintint_(ptr @139, i32 %3, i32 435)
  %5 = load i1, ptr %result, align 1
  %6 = zext i1 %5 to i32
  %7 = and i32 %6, %4
  %tobool = icmp ne i32 %7, 0
  store i1 %tobool, ptr %result, align 1
  %8 = call i1 @_testNamedParameters_bool__()
  %9 = load i1, ptr %result, align 1
  %10 = and i1 %9, %8
  %tobool1 = icmp ne i1 %10, false
  store i1 %tobool1, ptr %result, align 1
  %11 = load %MyStruct, ptr %my, align 4
  %12 = call i32 @_TotalByValue_int_MyStruct_(%MyStruct %11)
  %13 = call i32 @_Test_int_charPtrintint_(ptr @140, i32 %12, i32 5)
  %14 = load i1, ptr %result, align 1
  %15 = zext i1 %14 to i32
  %16 = and i32 %15, %13
  %tobool2 = icmp ne i32 %16, 0
  store i1 %tobool2, ptr %result, align 1
  %17 = call i32 @_TotalByPointer_int_MyStructPtr_(ptr %my)
  %18 = call i32 @_Test_int_charPtrintint_(ptr @141, i32 %17, i32 5)
  %19 = load i1, ptr %result, align 1
  %20 = zext i1 %19 to i32
  %21 = and i32 %20, %18
  %tobool3 = icmp ne i32 %21, 0
  store i1 %tobool3, ptr %result, align 1
  %22 = call i32 @_Read_int_MyStruct1Ptr_(ptr %struct1)
  %23 = call i32 @_Test_int_charPtrintint_(ptr @142, i32 %22, i32 1)
  %24 = load i1, ptr %result, align 1
  %25 = zext i1 %24 to i32
  %26 = and i32 %25, %23
  %tobool4 = icmp ne i32 %26, 0
  store i1 %tobool4, ptr %result, align 1
  %27 = call i32 @_Read_int_MyStruct2Ptr_(ptr %struct2)
  %28 = call i32 @_Test_int_charPtrintint_(ptr @143, i32 %27, i32 2)
  %29 = load i1, ptr %result, align 1
  %30 = zext i1 %29 to i32
  %31 = and i32 %30, %28
  %tobool5 = icmp ne i32 %31, 0
  store i1 %tobool5, ptr %result, align 1
  %32 = load %MyStruct1, ptr %struct1, align 4
  %33 = call i32 @_ReadExt_int_MyStruct1_(%MyStruct1 %32)
  %34 = call i32 @_Test_int_charPtrintint_(ptr @144, i32 %33, i32 1)
  %35 = load i1, ptr %result, align 1
  %36 = zext i1 %35 to i32
  %37 = and i32 %36, %34
  %tobool6 = icmp ne i32 %37, 0
  store i1 %tobool6, ptr %result, align 1
  %38 = load %MyStruct2, ptr %struct2, align 4
  %39 = call i32 @_ReadExt_int_MyStruct2_(%MyStruct2 %38)
  %40 = call i32 @_Test_int_charPtrintint_(ptr @145, i32 %39, i32 2)
  %41 = load i1, ptr %result, align 1
  %42 = zext i1 %41 to i32
  %43 = and i32 %42, %40
  %tobool7 = icmp ne i32 %43, 0
  store i1 %tobool7, ptr %result, align 1
  %44 = call i1 @_testDefaultSingleParam_bool__()
  %45 = load i1, ptr %result, align 1
  %46 = and i1 %45, %44
  %tobool8 = icmp ne i1 %46, false
  store i1 %tobool8, ptr %result, align 1
  %47 = call i1 @_testDefaultMultipleParams_bool__()
  %48 = load i1, ptr %result, align 1
  %49 = and i1 %48, %47
  %tobool9 = icmp ne i1 %49, false
  store i1 %tobool9, ptr %result, align 1
  %50 = call i1 @_testDefaultExpression_bool__()
  %51 = load i1, ptr %result, align 1
  %52 = and i1 %51, %50
  %tobool10 = icmp ne i1 %52, false
  store i1 %tobool10, ptr %result, align 1
  %53 = call i1 @_testDestructor_bool__()
  %54 = load i1, ptr %result, align 1
  %55 = and i1 %54, %53
  %tobool11 = icmp ne i1 %55, false
  store i1 %tobool11, ptr %result, align 1
  %56 = call i1 @_testBuiltinIdentifiers_bool__()
  %57 = load i1, ptr %result, align 1
  %58 = and i1 %57, %56
  %tobool12 = icmp ne i1 %58, false
  store i1 %tobool12, ptr %result, align 1
  %59 = call i1 @_testTypeof_bool__()
  %60 = load i1, ptr %result, align 1
  %61 = and i1 %60, %59
  %tobool13 = icmp ne i1 %61, false
  store i1 %tobool13, ptr %result, align 1
  %62 = call i1 @_testNameof_bool__()
  %63 = load i1, ptr %result, align 1
  %64 = and i1 %63, %62
  %tobool14 = icmp ne i1 %64, false
  store i1 %tobool14, ptr %result, align 1
  %65 = call i1 @_testNamespace_bool__()
  %66 = load i1, ptr %result, align 1
  %67 = and i1 %66, %65
  %tobool15 = icmp ne i1 %67, false
  store i1 %tobool15, ptr %result, align 1
  %68 = call i1 @_testNestedNamespace_bool__()
  %69 = load i1, ptr %result, align 1
  %70 = and i1 %69, %68
  %tobool16 = icmp ne i1 %70, false
  store i1 %tobool16, ptr %result, align 1
  %71 = call i1 @_testNestedUsing_bool__()
  %72 = load i1, ptr %result, align 1
  %73 = and i1 %72, %71
  %tobool17 = icmp ne i1 %73, false
  store i1 %tobool17, ptr %result, align 1
  %74 = call i1 @_testLocalUsing_bool__()
  %75 = load i1, ptr %result, align 1
  %76 = and i1 %75, %74
  %tobool18 = icmp ne i1 %76, false
  store i1 %tobool18, ptr %result, align 1
  %77 = call i1 @_testLocalUsingScoped_bool__()
  %78 = load i1, ptr %result, align 1
  %79 = and i1 %78, %77
  %tobool19 = icmp ne i1 %79, false
  store i1 %tobool19, ptr %result, align 1
  %80 = call i1 @_testForwardFunction_bool__()
  %81 = load i1, ptr %result, align 1
  %82 = and i1 %81, %80
  %tobool20 = icmp ne i1 %82, false
  store i1 %tobool20, ptr %result, align 1
  %83 = call i1 @_testMutualRecursion_bool__()
  %84 = load i1, ptr %result, align 1
  %85 = and i1 %84, %83
  %tobool21 = icmp ne i1 %85, false
  store i1 %tobool21, ptr %result, align 1
  %86 = call i1 @_testForwardFunctionWithStruct_bool__()
  %87 = load i1, ptr %result, align 1
  %88 = and i1 %87, %86
  %tobool22 = icmp ne i1 %88, false
  store i1 %tobool22, ptr %result, align 1
  %89 = call i1 @_testForwardInStruct_bool__()
  %90 = load i1, ptr %result, align 1
  %91 = and i1 %90, %89
  %tobool23 = icmp ne i1 %91, false
  store i1 %tobool23, ptr %result, align 1
  %92 = call i1 @_testReturnBlock_bool__()
  %93 = load i1, ptr %result, align 1
  %94 = and i1 %93, %92
  %tobool24 = icmp ne i1 %94, false
  store i1 %tobool24, ptr %result, align 1
  %95 = call i1 @_testAssertReturnBlock_bool__()
  %96 = load i1, ptr %result, align 1
  %97 = and i1 %96, %95
  %tobool25 = icmp ne i1 %97, false
  store i1 %tobool25, ptr %result, align 1
  %98 = call i1 @_testExplicitIntTypes_bool__()
  %99 = load i1, ptr %result, align 1
  %100 = and i1 %99, %98
  %tobool26 = icmp ne i1 %100, false
  store i1 %tobool26, ptr %result, align 1
  %101 = call i1 @_testNullConditional_bool__()
  %102 = load i1, ptr %result, align 1
  %103 = and i1 %102, %101
  %tobool27 = icmp ne i1 %103, false
  store i1 %tobool27, ptr %result, align 1
  %104 = call i1 @_testNullCoalescing_bool__()
  %105 = load i1, ptr %result, align 1
  %106 = and i1 %105, %104
  %tobool28 = icmp ne i1 %106, false
  store i1 %tobool28, ptr %result, align 1
  %107 = call i1 @_testDefault_bool__()
  %108 = load i1, ptr %result, align 1
  %109 = and i1 %108, %107
  %tobool29 = icmp ne i1 %109, false
  store i1 %tobool29, ptr %result, align 1
  %110 = call i32 @_testNewDelete_int__()
  %111 = call i32 @_Test_int_charPtrintint_(ptr @146, i32 %110, i32 42)
  %112 = load i1, ptr %result, align 1
  %113 = zext i1 %112 to i32
  %114 = and i32 %113, %111
  %tobool30 = icmp ne i32 %114, 0
  store i1 %tobool30, ptr %result, align 1
  %115 = call i32 @_testNewArray_int__()
  %116 = call i32 @_Test_int_charPtrintint_(ptr @147, i32 %115, i32 99)
  %117 = load i1, ptr %result, align 1
  %118 = zext i1 %117 to i32
  %119 = and i32 %118, %116
  %tobool31 = icmp ne i32 %119, 0
  store i1 %tobool31, ptr %result, align 1
  %120 = call i32 @_testNewWithConstructor_int__()
  %121 = call i32 @_Test_int_charPtrintint_(ptr @148, i32 %120, i32 123)
  %122 = load i1, ptr %result, align 1
  %123 = zext i1 %122 to i32
  %124 = and i32 %123, %121
  %tobool32 = icmp ne i32 %124, 0
  store i1 %tobool32, ptr %result, align 1
  %125 = call i1 @_testStringType_bool__()
  %126 = load i1, ptr %result, align 1
  %127 = and i1 %126, %125
  %tobool33 = icmp ne i1 %127, false
  store i1 %tobool33, ptr %result, align 1
  %128 = call i1 @_testStringLiteral_bool__()
  %129 = load i1, ptr %result, align 1
  %130 = and i1 %129, %128
  %tobool34 = icmp ne i1 %130, false
  store i1 %tobool34, ptr %result, align 1
  %131 = call i1 @_testFormatString_bool__()
  %132 = load i1, ptr %result, align 1
  %133 = and i1 %132, %131
  %tobool35 = icmp ne i1 %133, false
  store i1 %tobool35, ptr %result, align 1
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %134 = load i1, ptr %result, align 1
  br i1 %134, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @149)
  ret i32 0

ifResume:                                         ; preds = %ifCondition
  ret i32 1
}

declare void @printf(ptr, ...)

declare ptr @malloc(i64)

declare void @free(ptr)

declare i32 @strlen(ptr)

declare i32 @strcmp(ptr, ptr)

define internal %__iface_fat_ptr @__strconcat(ptr %ptrs, ptr %lens, i32 %count) {
entry:
  %total = alloca i32, align 4
  %buf = alloca ptr, align 8
  %dst = alloca ptr, align 8
  %idx = alloca i32, align 4
  store i32 0, ptr %total, align 4
  store i32 0, ptr %idx, align 4
  br label %sum.cond

sum.cond:                                         ; preds = %sum.body, %entry
  %0 = load i32, ptr %idx, align 4
  %1 = icmp slt i32 %0, %count
  br i1 %1, label %sum.body, label %alloc

sum.body:                                         ; preds = %sum.cond
  %2 = load i32, ptr %idx, align 4
  %3 = getelementptr i32, ptr %lens, i32 %2
  %4 = load i32, ptr %3, align 4
  %5 = load i32, ptr %total, align 4
  %6 = add i32 %5, %4
  store i32 %6, ptr %total, align 4
  %7 = add i32 %2, 1
  store i32 %7, ptr %idx, align 4
  br label %sum.cond

alloc:                                            ; preds = %sum.cond
  %8 = load i32, ptr %total, align 4
  %9 = sext i32 %8 to i64
  %10 = add i64 %9, 1
  %buf1 = call ptr @malloc(i64 %10)
  store ptr %buf1, ptr %buf, align 8
  store ptr %buf1, ptr %dst, align 8
  store i32 0, ptr %idx, align 4
  br label %cpy.cond

cpy.cond:                                         ; preds = %cpy.next, %alloc
  %11 = load i32, ptr %idx, align 4
  %12 = icmp slt i32 %11, %count
  br i1 %12, label %cpy.body, label %null

cpy.body:                                         ; preds = %cpy.cond
  %13 = load i32, ptr %idx, align 4
  %14 = getelementptr ptr, ptr %ptrs, i32 %13
  %15 = load ptr, ptr %14, align 8
  %16 = getelementptr i32, ptr %lens, i32 %13
  %17 = load i32, ptr %16, align 4
  %j = alloca i32, align 4
  store i32 0, ptr %j, align 4
  br label %b.cond

cpy.next:                                         ; preds = %b.cond
  %18 = load i32, ptr %idx, align 4
  %19 = getelementptr i32, ptr %lens, i32 %18
  %20 = load i32, ptr %19, align 4
  %21 = load ptr, ptr %dst, align 8
  %22 = sext i32 %20 to i64
  %23 = getelementptr i8, ptr %21, i64 %22
  store ptr %23, ptr %dst, align 8
  %24 = add i32 %18, 1
  store i32 %24, ptr %idx, align 4
  br label %cpy.cond

null:                                             ; preds = %cpy.cond
  %25 = load ptr, ptr %dst, align 8
  store i8 0, ptr %25, align 1
  %slraw = call ptr @malloc(i64 ptrtoint (ptr getelementptr (%__StrLit, ptr null, i32 1) to i64))
  %26 = getelementptr inbounds %__StrLit, ptr %slraw, i32 0, i32 0
  %27 = load ptr, ptr %buf, align 8
  store ptr %27, ptr %26, align 8
  %28 = getelementptr inbounds %__StrLit, ptr %slraw, i32 0, i32 1
  %29 = load i32, ptr %total, align 4
  store i32 %29, ptr %28, align 4
  %30 = insertvalue %__iface_fat_ptr { ptr @__StrLit_IReadOnlyString_vtable, ptr undef }, ptr %slraw, 1
  ret %__iface_fat_ptr %30

b.cond:                                           ; preds = %b.body, %cpy.body
  %31 = load i32, ptr %j, align 4
  %32 = icmp slt i32 %31, %17
  br i1 %32, label %b.body, label %cpy.next

b.body:                                           ; preds = %b.cond
  %33 = load i32, ptr %j, align 4
  %34 = load ptr, ptr %dst, align 8
  %35 = getelementptr i8, ptr %15, i32 %33
  %36 = load i8, ptr %35, align 1
  %37 = getelementptr i8, ptr %34, i32 %33
  store i8 %36, ptr %37, align 1
  %38 = add i32 %33, 1
  store i32 %38, ptr %j, align 4
  br label %b.cond
}
