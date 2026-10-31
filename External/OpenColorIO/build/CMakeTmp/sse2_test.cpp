
    #include <emmintrin.h>

    int main() 
    { 
        __m128d a, b;
        double vals[2] = {0};
        a = _mm_loadu_pd (vals);
        b = _mm_add_pd (a,a);
        _mm_storeu_pd (vals,b);
        return (0);
    }
