
    #include <immintrin.h>

    int main() 
    {
        __m256i a = _mm256_set_epi32(1, 2, 3, 4, 5, 6, 7, 8);
        __m256i b = _mm256_set_epi32(8, 7, 6, 5, 4, 3, 2, 1);
        __m256i result = _mm256_add_epi32(a, b);

        __m256 result_f16c = _mm256_cvtph_ps(_mm_set_epi16(1, 2, 3, 4, 5, 6, 7, 8));

        __m256 result_fma = _mm256_fmadd_ps(
            _mm256_set_ps(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0),
            _mm256_set_ps(8.0, 7.0, 6.0, 5.0, 4.0, 3.0, 2.0, 1.0),
            _mm256_set_ps(1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0)
        );

        return 0;
    }
