; ModuleID = 'MyCompiler'
source_filename = "MyCompiler"

%Storage__int = type { i32 }
%Storage__float = type { float }
%Storage__MyInt = type { %MyInt }
%MyInt = type { i32 }
%Pair__int = type { i32, i32 }
%Pair__float = type { float, float }
%__iface_fat_ptr = type { ptr, ptr }

@0 = private unnamed_addr constant [18 x i8] c"int default = %d\0A\00", align 1
@1 = private unnamed_addr constant [22 x i8] c"Created Storage<int>\0A\00", align 1
@Storage__int_Container__int_vtable = internal constant [2 x ptr] [ptr @_Get_int_Storage__intPtr_, ptr @_Set_void_Storage__intPtrint_]
@2 = private unnamed_addr constant [35 x i8] c"Called Parse via extension method\0A\00", align 1
@3 = private unnamed_addr constant [24 x i8] c"Created Storage<float>\0A\00", align 1
@4 = private unnamed_addr constant [24 x i8] c"Created Storage<MyInt>\0A\00", align 1
@5 = private unnamed_addr constant [19 x i8] c"Created Pair<int>\0A\00", align 1
@6 = private unnamed_addr constant [21 x i8] c"Created Pair<float>\0A\00", align 1

define %Storage__int @_Storage__int_Storage__int__() {
entry:
  ret %Storage__int zeroinitializer
}

define %Storage__float @_Storage__float_Storage__float__() {
entry:
  ret %Storage__float zeroinitializer
}

define %Storage__MyInt @_Storage__MyInt_Storage__MyInt__() {
entry:
  %0 = call %MyInt @_MyInt_MyInt__()
  %1 = insertvalue %Storage__MyInt undef, %MyInt %0, 0
  ret %Storage__MyInt %1
}

define %Pair__int @_Pair__int_Pair__int__() {
entry:
  ret %Pair__int zeroinitializer
}

define %Pair__float @_Pair__float_Pair__float__() {
entry:
  ret %Pair__float zeroinitializer
}

define %MyInt @_MyInt_MyInt__() {
entry:
  ret %MyInt { i32 5 }
}

define void @main() {
entry:
  %n = alloca i32, align 4
  store i32 0, ptr %n, align 4
  %0 = load i32, ptr %n, align 4
  call void (ptr, ...) @printf(ptr @0, i32 %0)
  %1 = call %Storage__int @_Storage__int_Storage__int__()
  %intStorage = alloca %Storage__int, align 8
  store %Storage__int %1, ptr %intStorage, align 4
  call void (ptr, ...) @printf(ptr @1)
  %2 = alloca %__iface_fat_ptr, align 8
  %3 = getelementptr inbounds %__iface_fat_ptr, ptr %2, i32 0, i32 0
  store ptr @Storage__int_Container__int_vtable, ptr %3, align 8
  %4 = getelementptr inbounds %__iface_fat_ptr, ptr %2, i32 0, i32 1
  store ptr %intStorage, ptr %4, align 8
  call void @_Parse__int_void_Container__intPtr_(ptr %2)
  call void (ptr, ...) @printf(ptr @2)
  %5 = call %Storage__float @_Storage__float_Storage__float__()
  %floatStorage = alloca %Storage__float, align 8
  store %Storage__float %5, ptr %floatStorage, align 4
  call void (ptr, ...) @printf(ptr @3)
  %6 = call %Storage__MyInt @_Storage__MyInt_Storage__MyInt__()
  %myIntStorage = alloca %Storage__MyInt, align 8
  store %Storage__MyInt %6, ptr %myIntStorage, align 4
  call void (ptr, ...) @printf(ptr @4)
  %7 = call %Pair__int @_Pair__int_Pair__int__()
  %intPair = alloca %Pair__int, align 8
  store %Pair__int %7, ptr %intPair, align 4
  call void (ptr, ...) @printf(ptr @5)
  %8 = call %Pair__float @_Pair__float_Pair__float__()
  %floatPair = alloca %Pair__float, align 8
  store %Pair__float %8, ptr %floatPair, align 4
  call void (ptr, ...) @printf(ptr @6)
  ret void
}

declare void @printf(ptr, ...)

define i32 @_Get_int_Storage__intPtr_(ptr %Storage__int__) {
entry:
  %0 = getelementptr inbounds %Storage__int, ptr %Storage__int__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  ret i32 %1
}

define void @_Set_void_Storage__intPtrint_(ptr %Storage__int__, i32 %value) {
entry:
  %0 = getelementptr inbounds %Storage__int, ptr %Storage__int__, i32 0, i32 0
  %1 = load i32, ptr %0, align 4
  store i32 %value, ptr %0, align 4
  ret void
}

define float @_Get_float_Storage__floatPtr_(ptr %Storage__float__) {
entry:
  %0 = getelementptr inbounds %Storage__float, ptr %Storage__float__, i32 0, i32 0
  %1 = load float, ptr %0, align 4
  ret float %1
}

define void @_Set_void_Storage__floatPtrfloat_(ptr %Storage__float__, float %value) {
entry:
  %0 = getelementptr inbounds %Storage__float, ptr %Storage__float__, i32 0, i32 0
  %1 = load float, ptr %0, align 4
  store float %value, ptr %0, align 4
  ret void
}

define %MyInt @_Get_MyInt_Storage__MyIntPtr_(ptr %Storage__MyInt__) {
entry:
  %0 = getelementptr inbounds %Storage__MyInt, ptr %Storage__MyInt__, i32 0, i32 0
  %1 = load %MyInt, ptr %0, align 4
  ret %MyInt %1
}

define void @_Set_void_Storage__MyIntPtrMyInt_(ptr %Storage__MyInt__, %MyInt %value) {
entry:
  %0 = getelementptr inbounds %Storage__MyInt, ptr %Storage__MyInt__, i32 0, i32 0
  %1 = load %MyInt, ptr %0, align 4
  store %MyInt %value, ptr %0, align 4
  ret void
}

define void @_Parse__int_void_Container__intPtr_(ptr %container) {
entry:
  %0 = getelementptr inbounds %__iface_fat_ptr, ptr %container, i32 0, i32 0
  %1 = load ptr, ptr %0, align 8
  %2 = getelementptr inbounds %__iface_fat_ptr, ptr %container, i32 0, i32 1
  %3 = load ptr, ptr %2, align 8
  %4 = getelementptr ptr, ptr %1, i32 0
  %5 = load ptr, ptr %4, align 8
  %6 = call i32 %5(ptr %3)
  %value = alloca i32, align 4
  store i32 %6, ptr %value, align 4
  ret void
}
