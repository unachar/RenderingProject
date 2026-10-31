
    #include <nmmintrin.h>
    
    int main() 
    { 
        _mm_cmpgt_epi64(_mm_set_epi64x(5, 10), _mm_set_epi64x(8, 5)); 
        return 0; 
    }
