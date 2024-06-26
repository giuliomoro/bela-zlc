/***** FFTConvolver.cpp *****/

#include "FFTConvolver.h"

#define CHECK_WRITE_MUTEX
#define LOCK_WRITE_MUTEX
#define LOCK_QUEUE_MUTEX
RtMutex FFTConvolver::writeMutex;

// Constructor taking the path of a file to load
FFTConvolver::FFTConvolver(int fftSize, std::vector<float>& h, int k, std::vector<float>& x, std::vector<float>& y, int idx)
{
	// FFT size must always be twice as large as block of samples
	assert (fftSize == 2 * h.size()); 
	
	setup(fftSize, h, k, x, y, idx);
}

// Load an audio file from the given filename. Returns true on success.
bool FFTConvolver::setup(int fftSize, std::vector<float>& h, int k, std::vector<float>& x, std::vector<float>& y, int idx)
{
	// store public member values
	fftSize_ = fftSize;
	k_ = k;
	x_ = &x;
	y_ = &y;
	idx_ = idx;
	outPointer_ = k_;

	// setup the FFT objects
	fftX = std::make_shared<Fft>();
	fftH = std::make_shared<Fft>();
	fftBuffer = std::make_shared<Fft>();
	fftX->setup(fftSize_);
	fftH->setup(fftSize_);
	fftBuffer->setup(fftSize_);

	// load the impulse response block 
	for (int n = 0; n < fftSize_; n++)
	{
		if (n < fftSize_/2){
			fftH->td(n) = h[n];
		}
		else
			fftH->td(n) = 0.0;
	}
	
	// compute frequency response
	fftH->fft();
	
	return true;
}

bool FFTConvolver::isQueued()
{
	return queued_;
}

int FFTConvolver::getFftSize()
{
	return fftSize_;
}

void FFTConvolver::queue(unsigned int inPointer, bool bypass)
{
#ifdef LOCK_QUEUE_MUTEX
	if(queueMutex->try_lock())
#else
	if(!isQueued()) // softer check
#endif // LOCK_QUEUE_MUTEX
	{
		inPointer_ = inPointer;
		queued_ = true;
		bypass_ = bypass;
#ifdef LOCK_QUEUE_MUTEX
		queueMutex->unlock();
#endif // LOCK_QUEUE_MUTEX
	} else {
		rt_printf("not ready %d\n", idx_);
	}
}

// Apply the filter H to an input block x in the frequency domain
void FFTConvolver::process()
{
	if (!bypass_)
	{
#ifdef LOCK_QUEUE_MUTEX
		queueMutex->lock();
#endif // LOCK_QUEUE_MUTEX
		// first grab fftsize/2 samples from the input circular buffer 
		for (int n = 0; n < fftSize_; n++)
		{
			if (n < fftSize_/2)
			{
				int circularBufferIndex = (inPointer_ + n - (fftSize_/2) + x_->size()) % x_->size();
				fftX->td(n) = x_->data()[circularBufferIndex];
			}
			else
			{
				fftX->td(n) = 0.0;
			}
		}
		
		// compute fft of the input block
		fftX->fft();
	
		// complex multiplication to apply filter in freq. domain
		for (int n = 0; n < fftSize_; n++) 
		{
			if (n < fftSize_/2)
			{
				fftBuffer->fdr(n) = (fftX->fdr(n) * fftH->fdr(n)) - (fftX->fdi(n) * fftH->fdi(n));
				fftBuffer->fdi(n) = (fftX->fdi(n) * fftH->fdr(n)) + (fftX->fdr(n) * fftH->fdi(n));
			}
			else
			{
				fftBuffer->fdr(n) = fftBuffer->fdr(fftSize_ - n - 1);
				fftBuffer->fdi(n) = fftBuffer->fdr(fftSize_ - n - 1);
			}
		}
		
		// compute time domain output with IFFT
		fftBuffer->ifft();
		
		// move the time domain output samples into the output buffer
#ifdef LOCK_WRITE_MUTEX
#ifdef CHECK_WRITE_MUTEX
		bool locked = writeMutex.try_lock();
		if(!locked) {
			rt_printf("waiting to lock writeMutex %d\n", idx_);
			writeMutex.lock();
		}
#else
		writeMutex.lock();
#endif
#endif // LOCK_WRITE_MUTEX
		for(int n = 0; n < fftSize_; n++) {
			int circularBufferIndex = (outPointer_ + n + y_->size()) % y_->size();
			y_->data()[circularBufferIndex] += fftBuffer->td(n);
		}
#ifdef LOCK_WRITE_MUTEX
		writeMutex.unlock();
#endif // LOCK_WRITE_MUTEX
#ifdef LOCK_QUEUE_MUTEX
		queueMutex->unlock();
#endif // LOCK_QUEUE_MUTEX
	}
	
	// update the write pointer (even on bypass)
	outPointer_ = (outPointer_ + (fftSize_/2)) % y_->size();
	
	queued_ = false;
}
	