
    #include <iostream>
    #include <immintrin.h>

    int main() 
    {
        // Create two arrays of floats
        float a[8] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
        float b[8] = {2.0, 4.0, 6.0, 8.0, 10.0, 12.0, 14.0, 16.0};

        _mm256_add_ps(_mm256_load_ps(a), _mm256_load_ps(b));
        return 0; 
    }
