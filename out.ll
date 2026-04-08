; ModuleID = 'MyCompiler'
source_filename = "MyCompiler"

%__StrLit = type { ptr, i32 }
%Vec2 = type { float, float }
%Fraction = type { i32, i32 }
%Counter = type { i32 }

@0 = private unnamed_addr constant [12 x i8] c"%s passed.\0A\00", align 1
@1 = private unnamed_addr constant [33 x i8] c"%s FAILED (expected %d got %d).\0A\00", align 1
@2 = private unnamed_addr constant [33 x i8] c"%s FAILED (expected %f got %f).\0A\00", align 1
@3 = private unnamed_addr constant [9 x i8] c"Vec2 + x\00", align 1
@4 = private unnamed_addr constant [9 x i8] c"Vec2 + y\00", align 1
@5 = private unnamed_addr constant [9 x i8] c"Vec2 - x\00", align 1
@6 = private unnamed_addr constant [9 x i8] c"Vec2 - y\00", align 1
@7 = private unnamed_addr constant [9 x i8] c"Vec2 * x\00", align 1
@8 = private unnamed_addr constant [9 x i8] c"Vec2 * y\00", align 1
@9 = private unnamed_addr constant [9 x i8] c"Vec2 / x\00", align 1
@10 = private unnamed_addr constant [9 x i8] c"Vec2 / y\00", align 1
@11 = private unnamed_addr constant [16 x i8] c"Vec2 == (equal)\00", align 1
@12 = private unnamed_addr constant [20 x i8] c"Vec2 == (not equal)\00", align 1
@13 = private unnamed_addr constant [20 x i8] c"Vec2 != (not equal)\00", align 1
@14 = private unnamed_addr constant [16 x i8] c"Vec2 != (equal)\00", align 1
@15 = private unnamed_addr constant [11 x i8] c"Frac + num\00", align 1
@16 = private unnamed_addr constant [11 x i8] c"Frac + den\00", align 1
@17 = private unnamed_addr constant [11 x i8] c"Frac - num\00", align 1
@18 = private unnamed_addr constant [11 x i8] c"Frac - den\00", align 1
@19 = private unnamed_addr constant [10 x i8] c"Frac * ==\00", align 1
@20 = private unnamed_addr constant [8 x i8] c"Frac ==\00", align 1
@21 = private unnamed_addr constant [8 x i8] c"Frac !=\00", align 1
@22 = private unnamed_addr constant [8 x i8] c"Frac < \00", align 1
@23 = private unnamed_addr constant [8 x i8] c"Frac > \00", align 1
@24 = private unnamed_addr constant [16 x i8] c"Counter + value\00", align 1
@25 = private unnamed_addr constant [16 x i8] c"Counter - value\00", align 1
@26 = private unnamed_addr constant [16 x i8] c"Counter * value\00", align 1
@27 = private unnamed_addr constant [16 x i8] c"Counter == (eq)\00", align 1
@28 = private unnamed_addr constant [16 x i8] c"Counter == (ne)\00", align 1
@29 = private unnamed_addr constant [12 x i8] c"Counter <  \00", align 1
@30 = private unnamed_addr constant [15 x i8] c"Vec2 chain + x\00", align 1
@31 = private unnamed_addr constant [15 x i8] c"Vec2 chain + y\00", align 1

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

define %Vec2 @_Vec2_Vec2__() {
entry:
  ret %Vec2 zeroinitializer
}

define %Vec2 @"_operator+_Vec2_Vec2PtrVec2_"(ptr %Vec2__, %Vec2 %other) {
entry:
  %0 = call %Vec2 @_Vec2_Vec2__()
  %r = alloca %Vec2, align 8
  store %Vec2 %0, ptr %r, align 4
  %1 = getelementptr inbounds %Vec2, ptr %r, i32 0, i32 0
  %2 = load float, ptr %1, align 4
  %3 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 0
  %4 = load float, ptr %3, align 4
  %5 = extractvalue %Vec2 %other, 0
  %6 = fadd float %4, %5
  store float %6, ptr %1, align 4
  %7 = getelementptr inbounds %Vec2, ptr %r, i32 0, i32 1
  %8 = load float, ptr %7, align 4
  %9 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 1
  %10 = load float, ptr %9, align 4
  %11 = extractvalue %Vec2 %other, 1
  %12 = fadd float %10, %11
  store float %12, ptr %7, align 4
  %13 = load %Vec2, ptr %r, align 4
  ret %Vec2 %13
}

define %Vec2 @_operator-_Vec2_Vec2PtrVec2_(ptr %Vec2__, %Vec2 %other) {
entry:
  %0 = call %Vec2 @_Vec2_Vec2__()
  %r = alloca %Vec2, align 8
  store %Vec2 %0, ptr %r, align 4
  %1 = getelementptr inbounds %Vec2, ptr %r, i32 0, i32 0
  %2 = load float, ptr %1, align 4
  %3 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 0
  %4 = load float, ptr %3, align 4
  %5 = extractvalue %Vec2 %other, 0
  %6 = fsub float %4, %5
  store float %6, ptr %1, align 4
  %7 = getelementptr inbounds %Vec2, ptr %r, i32 0, i32 1
  %8 = load float, ptr %7, align 4
  %9 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 1
  %10 = load float, ptr %9, align 4
  %11 = extractvalue %Vec2 %other, 1
  %12 = fsub float %10, %11
  store float %12, ptr %7, align 4
  %13 = load %Vec2, ptr %r, align 4
  ret %Vec2 %13
}

define %Vec2 @"_operator*_Vec2_Vec2Ptrfloat_"(ptr %Vec2__, float %s) {
entry:
  %0 = call %Vec2 @_Vec2_Vec2__()
  %r = alloca %Vec2, align 8
  store %Vec2 %0, ptr %r, align 4
  %1 = getelementptr inbounds %Vec2, ptr %r, i32 0, i32 0
  %2 = load float, ptr %1, align 4
  %3 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 0
  %4 = load float, ptr %3, align 4
  %5 = fmul float %4, %s
  store float %5, ptr %1, align 4
  %6 = getelementptr inbounds %Vec2, ptr %r, i32 0, i32 1
  %7 = load float, ptr %6, align 4
  %8 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 1
  %9 = load float, ptr %8, align 4
  %10 = fmul float %9, %s
  store float %10, ptr %6, align 4
  %11 = load %Vec2, ptr %r, align 4
  ret %Vec2 %11
}

define %Vec2 @"_operator/_Vec2_Vec2Ptrfloat_"(ptr %Vec2__, float %s) {
entry:
  %0 = call %Vec2 @_Vec2_Vec2__()
  %r = alloca %Vec2, align 8
  store %Vec2 %0, ptr %r, align 4
  %1 = getelementptr inbounds %Vec2, ptr %r, i32 0, i32 0
  %2 = load float, ptr %1, align 4
  %3 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 0
  %4 = load float, ptr %3, align 4
  %5 = fdiv float %4, %s
  store float %5, ptr %1, align 4
  %6 = getelementptr inbounds %Vec2, ptr %r, i32 0, i32 1
  %7 = load float, ptr %6, align 4
  %8 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 1
  %9 = load float, ptr %8, align 4
  %10 = fdiv float %9, %s
  store float %10, ptr %6, align 4
  %11 = load %Vec2, ptr %r, align 4
  ret %Vec2 %11
}

define i1 @"_operator==_bool_Vec2PtrVec2_"(ptr %Vec2__, %Vec2 %other) {
entry:
  %0 = alloca i1, align 1
  %1 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 0
  %2 = load float, ptr %1, align 4
  %3 = extractvalue %Vec2 %other, 0
  %4 = fcmp oeq float %2, %3
  %tobool = icmp ne i1 %4, false
  store i1 %tobool, ptr %0, align 1
  br i1 %4, label %trueAND, label %resumeAND

resumeAND:                                        ; preds = %trueAND, %entry
  %5 = load i1, ptr %0, align 1
  ret i1 %5

trueAND:                                          ; preds = %entry
  %6 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 1
  %7 = load float, ptr %6, align 4
  %8 = extractvalue %Vec2 %other, 1
  %9 = fcmp oeq float %7, %8
  %10 = and i1 %4, %9
  %tobool1 = icmp ne i1 %10, false
  store i1 %tobool1, ptr %0, align 1
  br label %resumeAND
}

define i1 @"_operator!=_bool_Vec2PtrVec2_"(ptr %Vec2__, %Vec2 %other) {
entry:
  %0 = alloca i1, align 1
  %1 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 0
  %2 = load float, ptr %1, align 4
  %3 = extractvalue %Vec2 %other, 0
  %4 = fcmp one float %2, %3
  %tobool = icmp ne i1 %4, false
  store i1 %tobool, ptr %0, align 1
  br i1 %4, label %resumeOR, label %falseOR

resumeOR:                                         ; preds = %falseOR, %entry
  %5 = load i1, ptr %0, align 1
  ret i1 %5

falseOR:                                          ; preds = %entry
  %6 = getelementptr inbounds %Vec2, ptr %Vec2__, i32 0, i32 1
  %7 = load float, ptr %6, align 4
  %8 = extractvalue %Vec2 %other, 1
  %9 = fcmp one float %7, %8
  %10 = or i1 %4, %9
  %tobool1 = icmp ne i1 %10, false
  store i1 %tobool1, ptr %0, align 1
  br label %resumeOR
}

define %Fraction @_Fraction_Fraction__() {
entry:
  ret %Fraction { i32 0, i32 1 }
}

define %Fraction @"_operator+_Fraction_FractionPtrFraction_"(ptr %Fraction__, %Fraction %other) {
entry:
  %0 = call %Fraction @_Fraction_Fraction__()
  %r = alloca %Fraction, align 8
  store %Fraction %0, ptr %r, align 4
  %1 = getelementptr inbounds %Fraction, ptr %r, i32 0, i32 0
  %2 = load i32, ptr %1, align 4
  %3 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 0
  %4 = load i32, ptr %3, align 4
  %5 = extractvalue %Fraction %other, 1
  %6 = mul i32 %4, %5
  %7 = extractvalue %Fraction %other, 0
  %8 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 1
  %9 = load i32, ptr %8, align 4
  %10 = mul i32 %7, %9
  %11 = add i32 %6, %10
  store i32 %11, ptr %1, align 4
  %12 = getelementptr inbounds %Fraction, ptr %r, i32 0, i32 1
  %13 = load i32, ptr %12, align 4
  %14 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 1
  %15 = load i32, ptr %14, align 4
  %16 = extractvalue %Fraction %other, 1
  %17 = mul i32 %15, %16
  store i32 %17, ptr %12, align 4
  %18 = load %Fraction, ptr %r, align 4
  ret %Fraction %18
}

define %Fraction @_operator-_Fraction_FractionPtrFraction_(ptr %Fraction__, %Fraction %other) {
entry:
  %0 = call %Fraction @_Fraction_Fraction__()
  %r = alloca %Fraction, align 8
  store %Fraction %0, ptr %r, align 4
  %1 = getelementptr inbounds %Fraction, ptr %r, i32 0, i32 0
  %2 = load i32, ptr %1, align 4
  %3 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 0
  %4 = load i32, ptr %3, align 4
  %5 = extractvalue %Fraction %other, 1
  %6 = mul i32 %4, %5
  %7 = extractvalue %Fraction %other, 0
  %8 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 1
  %9 = load i32, ptr %8, align 4
  %10 = mul i32 %7, %9
  %11 = sub i32 %6, %10
  store i32 %11, ptr %1, align 4
  %12 = getelementptr inbounds %Fraction, ptr %r, i32 0, i32 1
  %13 = load i32, ptr %12, align 4
  %14 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 1
  %15 = load i32, ptr %14, align 4
  %16 = extractvalue %Fraction %other, 1
  %17 = mul i32 %15, %16
  store i32 %17, ptr %12, align 4
  %18 = load %Fraction, ptr %r, align 4
  ret %Fraction %18
}

define %Fraction @"_operator*_Fraction_FractionPtrFraction_"(ptr %Fraction__, %Fraction %other) {
entry:
  %0 = call %Fraction @_Fraction_Fraction__()
  %r = alloca %Fraction, align 8
  store %Fraction %0, ptr %r, align 4
  %1 = getelementptr inbounds %Fraction, ptr %r, i32 0, i32 0
  %2 = load i32, ptr %1, align 4
  %3 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 0
  %4 = load i32, ptr %3, align 4
  %5 = extractvalue %Fraction %other, 0
  %6 = mul i32 %4, %5
  store i32 %6, ptr %1, align 4
  %7 = getelementptr inbounds %Fraction, ptr %r, i32 0, i32 1
  %8 = load i32, ptr %7, align 4
  %9 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 1
  %10 = load i32, ptr %9, align 4
  %11 = extractvalue %Fraction %other, 1
  %12 = mul i32 %10, %11
  store i32 %12, ptr %7, align 4
  %13 = load %Fraction, ptr %r, align 4
  ret %Fraction %13
}

define i1 @"_operator==_bool_FractionPtrFraction_"(ptr %Fraction__, %Fraction %other) {
entry:
  %0 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  %2 = extractvalue %Fraction %other, 1
  %3 = mul i32 %1, %2
  %4 = extractvalue %Fraction %other, 0
  %5 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 1
  %6 = load i32, ptr %5, align 4
  %7 = mul i32 %4, %6
  %8 = icmp eq i32 %3, %7
  ret i1 %8
}

define i1 @"_operator!=_bool_FractionPtrFraction_"(ptr %Fraction__, %Fraction %other) {
entry:
  %0 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  %2 = extractvalue %Fraction %other, 1
  %3 = mul i32 %1, %2
  %4 = extractvalue %Fraction %other, 0
  %5 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 1
  %6 = load i32, ptr %5, align 4
  %7 = mul i32 %4, %6
  %8 = icmp ne i32 %3, %7
  ret i1 %8
}

define i1 @"_operator<_bool_FractionPtrFraction_"(ptr %Fraction__, %Fraction %other) {
entry:
  %0 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  %2 = extractvalue %Fraction %other, 1
  %3 = mul i32 %1, %2
  %4 = extractvalue %Fraction %other, 0
  %5 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 1
  %6 = load i32, ptr %5, align 4
  %7 = mul i32 %4, %6
  %8 = icmp slt i32 %3, %7
  ret i1 %8
}

define i1 @"_operator>_bool_FractionPtrFraction_"(ptr %Fraction__, %Fraction %other) {
entry:
  %0 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  %2 = extractvalue %Fraction %other, 1
  %3 = mul i32 %1, %2
  %4 = extractvalue %Fraction %other, 0
  %5 = getelementptr inbounds %Fraction, ptr %Fraction__, i32 0, i32 1
  %6 = load i32, ptr %5, align 4
  %7 = mul i32 %4, %6
  %8 = icmp sgt i32 %3, %7
  ret i1 %8
}

define %Counter @_Counter_Counter__() {
entry:
  ret %Counter zeroinitializer
}

define %Counter @"_operator+_Counter_CounterPtrCounter_"(ptr %Counter__, %Counter %other) {
entry:
  %0 = call %Counter @_Counter_Counter__()
  %r = alloca %Counter, align 8
  store %Counter %0, ptr %r, align 4
  %1 = getelementptr inbounds %Counter, ptr %r, i32 0, i32 0
  %2 = load i32, ptr %1, align 4
  %3 = getelementptr inbounds %Counter, ptr %Counter__, i32 0, i32 0
  %4 = load i32, ptr %3, align 4
  %5 = extractvalue %Counter %other, 0
  %6 = add i32 %4, %5
  store i32 %6, ptr %1, align 4
  %7 = load %Counter, ptr %r, align 4
  ret %Counter %7
}

define %Counter @_operator-_Counter_CounterPtrCounter_(ptr %Counter__, %Counter %other) {
entry:
  %0 = call %Counter @_Counter_Counter__()
  %r = alloca %Counter, align 8
  store %Counter %0, ptr %r, align 4
  %1 = getelementptr inbounds %Counter, ptr %r, i32 0, i32 0
  %2 = load i32, ptr %1, align 4
  %3 = getelementptr inbounds %Counter, ptr %Counter__, i32 0, i32 0
  %4 = load i32, ptr %3, align 4
  %5 = extractvalue %Counter %other, 0
  %6 = sub i32 %4, %5
  store i32 %6, ptr %1, align 4
  %7 = load %Counter, ptr %r, align 4
  ret %Counter %7
}

define %Counter @"_operator*_Counter_CounterPtrint_"(ptr %Counter__, i32 %scale) {
entry:
  %0 = call %Counter @_Counter_Counter__()
  %r = alloca %Counter, align 8
  store %Counter %0, ptr %r, align 4
  %1 = getelementptr inbounds %Counter, ptr %r, i32 0, i32 0
  %2 = load i32, ptr %1, align 4
  %3 = getelementptr inbounds %Counter, ptr %Counter__, i32 0, i32 0
  %4 = load i32, ptr %3, align 4
  %5 = mul i32 %4, %scale
  store i32 %5, ptr %1, align 4
  %6 = load %Counter, ptr %r, align 4
  ret %Counter %6
}

define i1 @"_operator==_bool_CounterPtrCounter_"(ptr %Counter__, %Counter %other) {
entry:
  %0 = getelementptr inbounds %Counter, ptr %Counter__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  %2 = extractvalue %Counter %other, 0
  %3 = icmp eq i32 %1, %2
  ret i1 %3
}

define i1 @"_operator<_bool_CounterPtrCounter_"(ptr %Counter__, %Counter %other) {
entry:
  %0 = getelementptr inbounds %Counter, ptr %Counter__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  %2 = extractvalue %Counter %other, 0
  %3 = icmp slt i32 %1, %2
  ret i1 %3
}

define i32 @_TestBool_int_charPtrboolbool_(ptr %name, i1 %actual, i1 %expected) {
entry:
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %0 = icmp eq i1 %actual, %expected
  br i1 %0, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @0, ptr %name)
  ret i32 1

ifResume:                                         ; preds = %ifCondition
  %1 = zext i1 %expected to i32
  %2 = zext i1 %actual to i32
  call void (ptr, ...) @printf(ptr @1, ptr %name, i32 %1, i32 %2)
  ret i32 0
}

define i32 @_TestInt_int_charPtrintint_(ptr %name, i32 %actual, i32 %expected) {
entry:
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %0 = icmp eq i32 %actual, %expected
  br i1 %0, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @0, ptr %name)
  ret i32 1

ifResume:                                         ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @1, ptr %name, i32 %expected, i32 %actual)
  ret i32 0
}

define i32 @_TestFloat_int_charPtrfloatfloat_(ptr %name, float %actual, float %expected) {
entry:
  br label %ifCondition

ifCondition:                                      ; preds = %entry
  %0 = fcmp oeq float %actual, %expected
  br i1 %0, label %ifTrue, label %ifResume

ifTrue:                                           ; preds = %ifCondition
  call void (ptr, ...) @printf(ptr @0, ptr %name)
  ret i32 1

ifResume:                                         ; preds = %ifCondition
  %conv = fpext float %expected to double
  %conv1 = fpext float %actual to double
  call void (ptr, ...) @printf(ptr @2, ptr %name, double %conv, double %conv1)
  ret i32 0
}

define i32 @main() {
entry:
  %0 = call %Vec2 @_Vec2_Vec2__()
  %a = alloca %Vec2, align 8
  store %Vec2 %0, ptr %a, align 4
  %1 = getelementptr inbounds %Vec2, ptr %a, i32 0, i32 0
  %2 = load float, ptr %1, align 4
  store float 1.000000e+00, ptr %1, align 4
  %3 = getelementptr inbounds %Vec2, ptr %a, i32 0, i32 1
  %4 = load float, ptr %3, align 4
  store float 2.000000e+00, ptr %3, align 4
  %5 = call %Vec2 @_Vec2_Vec2__()
  %b = alloca %Vec2, align 8
  store %Vec2 %5, ptr %b, align 4
  %6 = getelementptr inbounds %Vec2, ptr %b, i32 0, i32 0
  %7 = load float, ptr %6, align 4
  store float 3.000000e+00, ptr %6, align 4
  %8 = getelementptr inbounds %Vec2, ptr %b, i32 0, i32 1
  %9 = load float, ptr %8, align 4
  store float 4.000000e+00, ptr %8, align 4
  %10 = load %Vec2, ptr %a, align 4
  %11 = load %Vec2, ptr %b, align 4
  %12 = alloca %Vec2, align 8
  store %Vec2 %10, ptr %12, align 4
  %13 = call %Vec2 @"_operator+_Vec2_Vec2PtrVec2_"(ptr %12, %Vec2 %11)
  %sum = alloca %Vec2, align 8
  store %Vec2 %13, ptr %sum, align 4
  %14 = getelementptr inbounds %Vec2, ptr %sum, i32 0, i32 0
  %15 = load float, ptr %14, align 4
  %16 = call i32 @_TestFloat_int_charPtrfloatfloat_(ptr @3, float %15, float 4.000000e+00)
  %17 = getelementptr inbounds %Vec2, ptr %sum, i32 0, i32 1
  %18 = load float, ptr %17, align 4
  %19 = call i32 @_TestFloat_int_charPtrfloatfloat_(ptr @4, float %18, float 6.000000e+00)
  %20 = load %Vec2, ptr %b, align 4
  %21 = load %Vec2, ptr %a, align 4
  %22 = alloca %Vec2, align 8
  store %Vec2 %20, ptr %22, align 4
  %23 = call %Vec2 @_operator-_Vec2_Vec2PtrVec2_(ptr %22, %Vec2 %21)
  %diff = alloca %Vec2, align 8
  store %Vec2 %23, ptr %diff, align 4
  %24 = getelementptr inbounds %Vec2, ptr %diff, i32 0, i32 0
  %25 = load float, ptr %24, align 4
  %26 = call i32 @_TestFloat_int_charPtrfloatfloat_(ptr @5, float %25, float 2.000000e+00)
  %27 = getelementptr inbounds %Vec2, ptr %diff, i32 0, i32 1
  %28 = load float, ptr %27, align 4
  %29 = call i32 @_TestFloat_int_charPtrfloatfloat_(ptr @6, float %28, float 2.000000e+00)
  %30 = load %Vec2, ptr %a, align 4
  %31 = alloca %Vec2, align 8
  store %Vec2 %30, ptr %31, align 4
  %32 = call %Vec2 @"_operator*_Vec2_Vec2Ptrfloat_"(ptr %31, float 3.000000e+00)
  %scaled = alloca %Vec2, align 8
  store %Vec2 %32, ptr %scaled, align 4
  %33 = getelementptr inbounds %Vec2, ptr %scaled, i32 0, i32 0
  %34 = load float, ptr %33, align 4
  %35 = call i32 @_TestFloat_int_charPtrfloatfloat_(ptr @7, float %34, float 3.000000e+00)
  %36 = getelementptr inbounds %Vec2, ptr %scaled, i32 0, i32 1
  %37 = load float, ptr %36, align 4
  %38 = call i32 @_TestFloat_int_charPtrfloatfloat_(ptr @8, float %37, float 6.000000e+00)
  %39 = load %Vec2, ptr %b, align 4
  %40 = alloca %Vec2, align 8
  store %Vec2 %39, ptr %40, align 4
  %41 = call %Vec2 @"_operator/_Vec2_Vec2Ptrfloat_"(ptr %40, float 2.000000e+00)
  %divided = alloca %Vec2, align 8
  store %Vec2 %41, ptr %divided, align 4
  %42 = getelementptr inbounds %Vec2, ptr %divided, i32 0, i32 0
  %43 = load float, ptr %42, align 4
  %44 = call i32 @_TestFloat_int_charPtrfloatfloat_(ptr @9, float %43, float 1.500000e+00)
  %45 = getelementptr inbounds %Vec2, ptr %divided, i32 0, i32 1
  %46 = load float, ptr %45, align 4
  %47 = call i32 @_TestFloat_int_charPtrfloatfloat_(ptr @10, float %46, float 2.000000e+00)
  %48 = call %Vec2 @_Vec2_Vec2__()
  %c = alloca %Vec2, align 8
  store %Vec2 %48, ptr %c, align 4
  %49 = getelementptr inbounds %Vec2, ptr %c, i32 0, i32 0
  %50 = load float, ptr %49, align 4
  store float 1.000000e+00, ptr %49, align 4
  %51 = getelementptr inbounds %Vec2, ptr %c, i32 0, i32 1
  %52 = load float, ptr %51, align 4
  store float 2.000000e+00, ptr %51, align 4
  %53 = load %Vec2, ptr %a, align 4
  %54 = load %Vec2, ptr %c, align 4
  %55 = alloca %Vec2, align 8
  store %Vec2 %53, ptr %55, align 4
  %56 = call i1 @"_operator==_bool_Vec2PtrVec2_"(ptr %55, %Vec2 %54)
  %57 = call i32 @_TestBool_int_charPtrboolbool_(ptr @11, i1 %56, i1 true)
  %58 = load %Vec2, ptr %a, align 4
  %59 = load %Vec2, ptr %b, align 4
  %60 = alloca %Vec2, align 8
  store %Vec2 %58, ptr %60, align 4
  %61 = call i1 @"_operator==_bool_Vec2PtrVec2_"(ptr %60, %Vec2 %59)
  %62 = call i32 @_TestBool_int_charPtrboolbool_(ptr @12, i1 %61, i1 false)
  %63 = load %Vec2, ptr %a, align 4
  %64 = load %Vec2, ptr %b, align 4
  %65 = alloca %Vec2, align 8
  store %Vec2 %63, ptr %65, align 4
  %66 = call i1 @"_operator!=_bool_Vec2PtrVec2_"(ptr %65, %Vec2 %64)
  %67 = call i32 @_TestBool_int_charPtrboolbool_(ptr @13, i1 %66, i1 true)
  %68 = load %Vec2, ptr %a, align 4
  %69 = load %Vec2, ptr %c, align 4
  %70 = alloca %Vec2, align 8
  store %Vec2 %68, ptr %70, align 4
  %71 = call i1 @"_operator!=_bool_Vec2PtrVec2_"(ptr %70, %Vec2 %69)
  %72 = call i32 @_TestBool_int_charPtrboolbool_(ptr @14, i1 %71, i1 false)
  %73 = call %Fraction @_Fraction_Fraction__()
  %half = alloca %Fraction, align 8
  store %Fraction %73, ptr %half, align 4
  %74 = getelementptr inbounds %Fraction, ptr %half, i32 0, i32 0
  %75 = load i32, ptr %74, align 4
  store i32 1, ptr %74, align 4
  %76 = getelementptr inbounds %Fraction, ptr %half, i32 0, i32 1
  %77 = load i32, ptr %76, align 4
  store i32 2, ptr %76, align 4
  %78 = call %Fraction @_Fraction_Fraction__()
  %third = alloca %Fraction, align 8
  store %Fraction %78, ptr %third, align 4
  %79 = getelementptr inbounds %Fraction, ptr %third, i32 0, i32 0
  %80 = load i32, ptr %79, align 4
  store i32 1, ptr %79, align 4
  %81 = getelementptr inbounds %Fraction, ptr %third, i32 0, i32 1
  %82 = load i32, ptr %81, align 4
  store i32 3, ptr %81, align 4
  %83 = call %Fraction @_Fraction_Fraction__()
  %twothirds = alloca %Fraction, align 8
  store %Fraction %83, ptr %twothirds, align 4
  %84 = getelementptr inbounds %Fraction, ptr %twothirds, i32 0, i32 0
  %85 = load i32, ptr %84, align 4
  store i32 2, ptr %84, align 4
  %86 = getelementptr inbounds %Fraction, ptr %twothirds, i32 0, i32 1
  %87 = load i32, ptr %86, align 4
  store i32 3, ptr %86, align 4
  %88 = load %Fraction, ptr %half, align 4
  %89 = load %Fraction, ptr %third, align 4
  %90 = alloca %Fraction, align 8
  store %Fraction %88, ptr %90, align 4
  %91 = call %Fraction @"_operator+_Fraction_FractionPtrFraction_"(ptr %90, %Fraction %89)
  %fsum = alloca %Fraction, align 8
  store %Fraction %91, ptr %fsum, align 4
  %92 = getelementptr inbounds %Fraction, ptr %fsum, i32 0, i32 0
  %93 = load i32, ptr %92, align 4
  %94 = call i32 @_TestInt_int_charPtrintint_(ptr @15, i32 %93, i32 5)
  %95 = getelementptr inbounds %Fraction, ptr %fsum, i32 0, i32 1
  %96 = load i32, ptr %95, align 4
  %97 = call i32 @_TestInt_int_charPtrintint_(ptr @16, i32 %96, i32 6)
  %98 = load %Fraction, ptr %half, align 4
  %99 = load %Fraction, ptr %third, align 4
  %100 = alloca %Fraction, align 8
  store %Fraction %98, ptr %100, align 4
  %101 = call %Fraction @_operator-_Fraction_FractionPtrFraction_(ptr %100, %Fraction %99)
  %fdiff = alloca %Fraction, align 8
  store %Fraction %101, ptr %fdiff, align 4
  %102 = getelementptr inbounds %Fraction, ptr %fdiff, i32 0, i32 0
  %103 = load i32, ptr %102, align 4
  %104 = call i32 @_TestInt_int_charPtrintint_(ptr @17, i32 %103, i32 1)
  %105 = getelementptr inbounds %Fraction, ptr %fdiff, i32 0, i32 1
  %106 = load i32, ptr %105, align 4
  %107 = call i32 @_TestInt_int_charPtrintint_(ptr @18, i32 %106, i32 6)
  %108 = load %Fraction, ptr %half, align 4
  %109 = load %Fraction, ptr %twothirds, align 4
  %110 = alloca %Fraction, align 8
  store %Fraction %108, ptr %110, align 4
  %111 = call %Fraction @"_operator*_Fraction_FractionPtrFraction_"(ptr %110, %Fraction %109)
  %fprod = alloca %Fraction, align 8
  store %Fraction %111, ptr %fprod, align 4
  %112 = call %Fraction @_Fraction_Fraction__()
  %oneThird = alloca %Fraction, align 8
  store %Fraction %112, ptr %oneThird, align 4
  %113 = getelementptr inbounds %Fraction, ptr %oneThird, i32 0, i32 0
  %114 = load i32, ptr %113, align 4
  store i32 1, ptr %113, align 4
  %115 = getelementptr inbounds %Fraction, ptr %oneThird, i32 0, i32 1
  %116 = load i32, ptr %115, align 4
  store i32 3, ptr %115, align 4
  %117 = load %Fraction, ptr %fprod, align 4
  %118 = load %Fraction, ptr %oneThird, align 4
  %119 = alloca %Fraction, align 8
  store %Fraction %117, ptr %119, align 4
  %120 = call i1 @"_operator==_bool_FractionPtrFraction_"(ptr %119, %Fraction %118)
  %121 = call i32 @_TestBool_int_charPtrboolbool_(ptr @19, i1 %120, i1 true)
  %122 = load %Fraction, ptr %half, align 4
  %123 = load %Fraction, ptr %half, align 4
  %124 = alloca %Fraction, align 8
  store %Fraction %122, ptr %124, align 4
  %125 = call i1 @"_operator==_bool_FractionPtrFraction_"(ptr %124, %Fraction %123)
  %126 = call i32 @_TestBool_int_charPtrboolbool_(ptr @20, i1 %125, i1 true)
  %127 = load %Fraction, ptr %half, align 4
  %128 = load %Fraction, ptr %third, align 4
  %129 = alloca %Fraction, align 8
  store %Fraction %127, ptr %129, align 4
  %130 = call i1 @"_operator!=_bool_FractionPtrFraction_"(ptr %129, %Fraction %128)
  %131 = call i32 @_TestBool_int_charPtrboolbool_(ptr @21, i1 %130, i1 true)
  %132 = load %Fraction, ptr %third, align 4
  %133 = load %Fraction, ptr %half, align 4
  %134 = alloca %Fraction, align 8
  store %Fraction %132, ptr %134, align 4
  %135 = call i1 @"_operator<_bool_FractionPtrFraction_"(ptr %134, %Fraction %133)
  %136 = call i32 @_TestBool_int_charPtrboolbool_(ptr @22, i1 %135, i1 true)
  %137 = load %Fraction, ptr %half, align 4
  %138 = load %Fraction, ptr %third, align 4
  %139 = alloca %Fraction, align 8
  store %Fraction %137, ptr %139, align 4
  %140 = call i1 @"_operator>_bool_FractionPtrFraction_"(ptr %139, %Fraction %138)
  %141 = call i32 @_TestBool_int_charPtrboolbool_(ptr @23, i1 %140, i1 true)
  %142 = call %Counter @_Counter_Counter__()
  %p = alloca %Counter, align 8
  store %Counter %142, ptr %p, align 4
  %143 = getelementptr inbounds %Counter, ptr %p, i32 0, i32 0
  %144 = load i32, ptr %143, align 4
  store i32 10, ptr %143, align 4
  %145 = call %Counter @_Counter_Counter__()
  %q = alloca %Counter, align 8
  store %Counter %145, ptr %q, align 4
  %146 = getelementptr inbounds %Counter, ptr %q, i32 0, i32 0
  %147 = load i32, ptr %146, align 4
  store i32 5, ptr %146, align 4
  %148 = load %Counter, ptr %p, align 4
  %149 = load %Counter, ptr %q, align 4
  %150 = alloca %Counter, align 8
  store %Counter %148, ptr %150, align 4
  %151 = call %Counter @"_operator+_Counter_CounterPtrCounter_"(ptr %150, %Counter %149)
  %csum = alloca %Counter, align 8
  store %Counter %151, ptr %csum, align 4
  %152 = load %Counter, ptr %p, align 4
  %153 = load %Counter, ptr %q, align 4
  %154 = alloca %Counter, align 8
  store %Counter %152, ptr %154, align 4
  %155 = call %Counter @_operator-_Counter_CounterPtrCounter_(ptr %154, %Counter %153)
  %cdiff = alloca %Counter, align 8
  store %Counter %155, ptr %cdiff, align 4
  %156 = load %Counter, ptr %q, align 4
  %157 = alloca %Counter, align 8
  store %Counter %156, ptr %157, align 4
  %158 = call %Counter @"_operator*_Counter_CounterPtrint_"(ptr %157, i32 4)
  %cmul = alloca %Counter, align 8
  store %Counter %158, ptr %cmul, align 4
  %159 = getelementptr inbounds %Counter, ptr %csum, i32 0, i32 0
  %160 = load i32, ptr %159, align 4
  %161 = call i32 @_TestInt_int_charPtrintint_(ptr @24, i32 %160, i32 15)
  %162 = getelementptr inbounds %Counter, ptr %cdiff, i32 0, i32 0
  %163 = load i32, ptr %162, align 4
  %164 = call i32 @_TestInt_int_charPtrintint_(ptr @25, i32 %163, i32 5)
  %165 = getelementptr inbounds %Counter, ptr %cmul, i32 0, i32 0
  %166 = load i32, ptr %165, align 4
  %167 = call i32 @_TestInt_int_charPtrintint_(ptr @26, i32 %166, i32 20)
  %168 = load %Counter, ptr %p, align 4
  %169 = load %Counter, ptr %p, align 4
  %170 = alloca %Counter, align 8
  store %Counter %168, ptr %170, align 4
  %171 = call i1 @"_operator==_bool_CounterPtrCounter_"(ptr %170, %Counter %169)
  %172 = call i32 @_TestBool_int_charPtrboolbool_(ptr @27, i1 %171, i1 true)
  %173 = load %Counter, ptr %p, align 4
  %174 = load %Counter, ptr %q, align 4
  %175 = alloca %Counter, align 8
  store %Counter %173, ptr %175, align 4
  %176 = call i1 @"_operator==_bool_CounterPtrCounter_"(ptr %175, %Counter %174)
  %177 = call i32 @_TestBool_int_charPtrboolbool_(ptr @28, i1 %176, i1 false)
  %178 = load %Counter, ptr %q, align 4
  %179 = load %Counter, ptr %p, align 4
  %180 = alloca %Counter, align 8
  store %Counter %178, ptr %180, align 4
  %181 = call i1 @"_operator<_bool_CounterPtrCounter_"(ptr %180, %Counter %179)
  %182 = call i32 @_TestBool_int_charPtrboolbool_(ptr @29, i1 %181, i1 true)
  %183 = load %Vec2, ptr %a, align 4
  %184 = load %Vec2, ptr %b, align 4
  %185 = alloca %Vec2, align 8
  store %Vec2 %183, ptr %185, align 4
  %186 = call %Vec2 @"_operator+_Vec2_Vec2PtrVec2_"(ptr %185, %Vec2 %184)
  %187 = load %Vec2, ptr %c, align 4
  %188 = alloca %Vec2, align 8
  store %Vec2 %186, ptr %188, align 4
  %189 = call %Vec2 @"_operator+_Vec2_Vec2PtrVec2_"(ptr %188, %Vec2 %187)
  %chain = alloca %Vec2, align 8
  store %Vec2 %189, ptr %chain, align 4
  %190 = getelementptr inbounds %Vec2, ptr %chain, i32 0, i32 0
  %191 = load float, ptr %190, align 4
  %192 = call i32 @_TestFloat_int_charPtrfloatfloat_(ptr @30, float %191, float 5.000000e+00)
  %193 = getelementptr inbounds %Vec2, ptr %chain, i32 0, i32 1
  %194 = load float, ptr %193, align 4
  %195 = call i32 @_TestFloat_int_charPtrfloatfloat_(ptr @31, float %194, float 8.000000e+00)
  ret i32 0
}

declare void @printf(ptr, ...)
