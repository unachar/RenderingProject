
    #include <smmintrin.h>

    int main() 
    { 
        _mm_blend_epi16(_mm_setzero_si128(), _mm_setzero_si128(), 0);
        return 0; 
    }
