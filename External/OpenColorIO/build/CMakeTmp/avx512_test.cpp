
    #include <immintrin.h>

    int main() {
        __m512i vec = _mm512_set1_epi32(42);
        return 0;
    }
