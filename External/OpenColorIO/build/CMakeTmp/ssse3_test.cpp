
    #include <tmmintrin.h>

    int main() 
    { 
        _mm_shuffle_epi8(_mm_setzero_si128(), _mm_setzero_si128()); 
        return 0; 
    }
