/***** ZLConvolver.cpp *****/

#include "ZLConvolver.h"
#include <libraries/AudioFile/AudioFile.h>

// Constructor taking the path of a file to load
ZLConvolver::ZLConvolver(int blockSize, int audioSampleRate, std::string impulseFilename, int maxKernelSize, bool random)
{
	setup(blockSize, audioSampleRate, impulseFilename, maxKernelSize, random);
}

bool ZLConvolver::setup(int blockSize, int audioSampleRate, std::string impulseFilename, int maxKernelSize, bool random)
{
	random_ = random;
	std::vector<float> impulsePlayer;
	int kernelSize = maxKernelSize;

	if (!random)
	{
		// Load our impulse response from file
		impulsePlayer = AudioFileUtilities::loadMono(impulseFilename);
		if (!impulsePlayer.size())
		{
			printf("Error loading impulse response file '%s'\n", impulseFilename.c_str());
			return false;
		}
		kernelSize = impulsePlayer.size();
		if(maxKernelSize)
			kernelSize = std::min(kernelSize, maxKernelSize);

		// Print some useful info
		printf("Loaded the impulse response file '%s' with %d frames (%.1f seconds)\n",
				  impulseFilename.c_str(), kernelSize,
				  kernelSize/ float(audioSampleRate));

	}

	// Set up the FFT and buffers

	// N_ = 32 is the smallest N such that
	// the FFT is faster than the direct form convolution,
	// but we cannot have it smaller than the blocksize
	N_ = std::max(32, blockSize * 4);

	// add some latency to give time to the extra threads to
	// perform their work after being scheduled
	// TODO: not sure this is the minimum possible value
	int addedLatency = 2 * N_;
	inputBuffer_.resize(kernelSize + addedLatency);
	outputBuffer_.resize(kernelSize + addedLatency);
	outputBufferReadPointer_ = outputBuffer_.size() - addedLatency;

	// Here we create an array of fftConvolvers
	// each has a separate block of the impulse response
	int k = 0; // starting position in the impulse response
	int samplesRead = 0;
	blocks_ = 0;
	std::vector<float> h;

	while (samplesRead < kernelSize)
	{
		int fftSize = 0;
		bool direct = false; // use direct form conv. for first block
		float value;

		// follow pattern: 2N, N, N, 2N, 2N, 4N, 4N, ...
		if (blocks_ == 0)
		{
			fftSize = 2 * N_;
			direct = true;
		}
		else if (blocks_ % 2 != 0)
			fftSize = (int)powf(2, (blocks_ / 2)) * N_;
		else
			fftSize = (int)powf(2, (blocks_ / 2) - 1) * N_;

		if (!random)
			value = impulsePlayer[samplesRead++];
		else
			value = randFloat(-0.1, 0.1);

		h.push_back(value);

		// when we read enough samples, create a convolver
		if ((samplesRead - k) == (fftSize / 2))
		{
			int priority = (int)basePriority_ - (blocks_);

			if (direct)
			{
				directConvolver_.setup(h, k, inputBuffer_, outputBuffer_);
			}
			else
			{
				// Note: actual FFT size is always twice as large as block size
				FFTConvolver convolver(fftSize, h, k, inputBuffer_, outputBuffer_, priority);
				fftConvolvers_.push_back(convolver);
				convolverBufferSamples_.push_back(0);
				convolverPriority_.push_back(priority);
				printf("n: %d  fftSize: %d. priority: %d samplesRead: %d  k: %d\n", blocks_, fftSize, priority, samplesRead, k);
			}

			blocks_++;
			h.clear(); // remove all elements from h
			k = samplesRead;
		}
	}

	// create threads for FFT convolutions
	// each convolver will have its own thread
	for (int n = 0; n < fftConvolvers_.size(); n++)
	{
		convolverThreads_.push_back(
			Bela_createAuxiliaryTask(
				convolverLauncher,
				convolverPriority_[n],
				"convolverLauncher",
				&fftConvolvers_[n]));
	}

	printf("Splitting impulse into %d blocks.\n", blocks_);

	return true;
}

float ZLConvolver::process(float in, int maxBlocks, float sparsity)
{
	// store input sample into input circular buffer
	inputBuffer_[inputBufferPointer_++] = in;
	if (inputBufferPointer_ >= inputBuffer_.size())
	{
		inputBufferPointer_ = 0;
	}

	// direct convolution
	//directConvolver_.process(inputBufferPointer_);

	// iterate over FFT convolutions
	for (int n = 2; n < fftConvolvers_.size(); n++)
	{
		// based on the GUI controls we may ignore some blocks in the filter
		bool bypass = false;
		if ((sparsity && n % (int)(((1 - sparsity) * (blocks_ / 2)) + 1) == 0) || n > maxBlocks)
			bypass = true;

		// when enough samples are loaded, we will launch the correct convolver threads
		if (++convolverBufferSamples_[n] == (fftConvolvers_[n].getFftSize() / 2))
		{
			fftConvolvers_[n].queue(inputBufferPointer_, bypass);
			Bela_scheduleAuxiliaryTask(convolverThreads_[n]);
			convolverBufferSamples_[n] = 0; // reset this convolver until buffer is full
		}
	}

	// Get the output sample from the output buffer
	float out = outputBuffer_[outputBufferReadPointer_];

	// Then clear the output sample in the buffer so it is ready for the next overlap-add
	//FFTConvolver::writeMutex.lock(); // TODO: may block and cause priority inversion
	outputBuffer_[outputBufferReadPointer_] = 0;
	//FFTConvolver::writeMutex.unlock();

	// Increment the read pointer in the output circular buffer
	outputBufferReadPointer_++;
	if (outputBufferReadPointer_ >= outputBuffer_.size())
		outputBufferReadPointer_ = 0;

	return out;
}
