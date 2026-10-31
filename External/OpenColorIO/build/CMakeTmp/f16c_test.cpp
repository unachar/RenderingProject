
    #include <immintrin.h>

    int main() 
    { 
        _mm_cvtph_ps(_mm_set1_epi16(0x3C00)); 
        return 0; 
    }
