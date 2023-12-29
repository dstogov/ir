#include <stdio.h>
#include <sys/time.h>

#define BAILOUT 16
#define MAX_ITERATIONS 1000

int iterate(double x, double y)
{
	double cr = y - 0.5;
	double ci = x;
	double zi = 0.0;
	double zr = 0.0;
	int i = 0;

	while(1) {
		i ++;
		double temp = zr * zi;
		double zr2 = zr * zr;
		double zi2 = zi * zi;
		zr = zr2 - zi2 + cr;
		zi = temp + temp + ci;
		if (zi2 + zr2 > BAILOUT)
			return i;
		if (i > MAX_ITERATIONS)
			return 0;
	}
	
}

void mandelbrot(void) {
	struct timeval aTv;
	gettimeofday(&aTv, NULL);
	long init_time = aTv.tv_sec;
	long init_usec = aTv.tv_usec;

	int x,y;
	for (y = -39; y < 39; y++) {
		fputc('\n', stdout);
		for (x = -39; x < 39; x++) {
			int i = iterate(x/40.0, y/40.0);
			if (i==0)
				fputc('*', stdout);
			else
				fputc(' ', stdout);
		}	
	}
	fputc('\n', stdout);
	
	gettimeofday(&aTv,NULL);
	double query_time = (aTv.tv_sec - init_time) + (double)(aTv.tv_usec - init_usec)/1000000.0;	
	printf ("C Elapsed %0.3f\n", query_time);
}

int main (int argc, const char * argv[]) {
	mandelbrot();
	mandelbrot();
    return 0;
}
