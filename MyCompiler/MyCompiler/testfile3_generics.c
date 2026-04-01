// Test generic struct template
struct Box<T> {
    T value = 0;
};

extern void main() {
    Box<MyInt> b = Box__MyInt();
}
