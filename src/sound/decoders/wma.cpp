/* Phaethon - A FLOSS resource explorer for BioWare's Aurora engine games
 *
 * Phaethon is the legal property of its developers, whose names
 * can be found in the AUTHORS file distributed with this source
 * distribution.
 *
 * Phaethon is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * Phaethon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Phaethon. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file
 *  Decoding Microsoft's Windows Media Audio.
 */

/* Based on the WMA implementation in FFmpeg (<https://ffmpeg.org/)>,
 * which is released under the terms of version 2 or later of the GNU
 * Lesser General Public License.
 *
 * The original copyright note in libavcodec/wma.c reads as follows:
 *
 * WMA compatible codec
 * Copyright (c) 2002-2007 The FFmpeg Project
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <cassert>
#include <cstring>

#include <vector>
#include <memory>

#include "src/common/util.h"
#include "src/common/maths.h"
#include "src/common/sinewindows.h"
#include "src/common/error.h"
#include "src/common/memreadstream.h"
#include "src/common/mdct.h"
#include "src/common/bitstream.h"
#include "src/common/huffman.h"
#include "src/common/types.h"
#include "src/common/ptrvector.h"

#include "src/sound/audiostream.h"

#include "src/sound/decoders/util.h"
#include "src/sound/decoders/pcm.h"
#include "src/sound/decoders/wma.h"
#include "src/sound/decoders/wmadata.h"

namespace Sound {

static inline void butterflyFloats(float *v1, float *v2, int len) {
	while (len-- > 0) {
		float t = *v1 - *v2;

		*v1++ += *v2;
		*v2++  = t;
	}
}

static inline void vectorFMulAdd(float *dst, const float *src0,
                          const float *src1, const float *src2, int len) {
	while (len-- > 0)
		*dst++ = *src0++ * *src1++ + *src2++;
}

static inline void vectorFMulReverse(float *dst, const float *src0,
                                     const float *src1, int len) {
	src1 += len - 1;

	while (len-- > 0)
		*dst++ = *src0++ * *src1--;
}

struct WMACoefHuffmanParam;

class WMACodec : public PacketizedAudioStream {
public:
	WMACodec(int version, uint32_t sampleRate, uint8_t channels,
	         uint32_t bitRate, uint32_t blockAlign, Common::SeekableReadStream *extraData = 0);
	~WMACodec();

	// AudioStream API
	int getChannels() const { return _channels; }
	int getRate() const { return _sampleRate; }
	bool endOfData() const { return _audStream->endOfData(); }
	bool endOfStream() const { return _audStream->endOfStream(); }
	size_t readBuffer(int16_t *buffer, const size_t numSamples) { return _audStream->readBuffer(buffer, numSamples); }

	// PacketizedAudioStream API
	void finish() { _audStream->finish(); }
	bool isFinished() const { return _audStream->isFinished(); }
	void queuePacket(Common::SeekableReadStream *data);

private:
	static const int kChannelsMax = 2; ///< Max number of channels we support.

	static const int kBlockBitsMin =  7; ///< Min number of bits in a block.
	static const int kBlockBitsMax = 11; ///< Max number of bits in a block.

	/** Max number of bytes in a block. */
	static const int kBlockSizeMax = (1 << kBlockBitsMax);

	static const int kBlockNBSizes = (kBlockBitsMax - kBlockBitsMin + 1);

	/** Max size of a superframe. */
	static const int kSuperframeSizeMax = 16384;

	/** Max size of a high band. */
	static const int kHighBandSizeMax = 16;

	/** Size of the noise table. */
	static const int kNoiseTabSize = 8192;

	/** Number of bits for the LSP power value. */
	static const int kLSPPowBits = 7;

	int _version; ///< WMA version.

	uint32_t _sampleRate; ///< Output sample rate.
	uint8_t  _channels;   ///< Output channel count.
	uint32_t _bitRate;    ///< Input bit rate.
	uint32_t _blockAlign; ///< Input block align.
	byte   _audioFlags; ///< Output flags.

	bool _useExpHuffman;       ///< Exponents in Huffman code? Otherwise, in LSP.
	bool _useBitReservoir;     ///< Is each frame packet a "superframe"?
	bool _useVariableBlockLen; ///< Are the block lengths variable?
	bool _useNoiseCoding;      ///< Should perceptual noise be added?

	bool _resetBlockLengths; ///< Do we need new block lengths?

	int _curFrame;       ///< The number of the frame we're currently in.
	int _frameLen;       ///< The frame length.
	int _frameLenBits;   ///< log2 of the frame length.
	int _blockSizeCount; ///< Number of block sizes.
	int _framePos;       ///< The position within the frame we're currently in.

	int _curBlock;         ///< The number of the block we're currently in.
	int _blockLen;         ///< Current block length.
	int _blockLenBits;     ///< log2 of current block length.
	int _nextBlockLenBits; ///< log2 of next block length.
	int _prevBlockLenBits; ///< log2 of previous block length.

	int _byteOffsetBits;

	// Coefficients
	int    _coefsStart;                       ///< First coded coef.
	int    _coefsEnd[kBlockNBSizes];          ///< Max number of coded coefficients.
	int    _exponentSizes[kBlockNBSizes];
	uint16_t _exponentBands[kBlockNBSizes][25];
	int    _highBandStart[kBlockNBSizes];     ///< Index of first coef in high band.
	int    _exponentHighSizes[kBlockNBSizes];
	int    _exponentHighBands[kBlockNBSizes][kHighBandSizeMax];

	std::unique_ptr<Common::Huffman> _coefHuffman[2]; ///< Coefficients Huffman codes.

	const WMACoefHuffmanParam *_coefHuffmanParam[2]; ///< Params for coef Huffman codes.

	std::unique_ptr<uint16_t[]> _coefHuffmanRunTable[2];   ///< Run table for the coef Huffman.
	std::unique_ptr<float[]>  _coefHuffmanLevelTable[2]; ///< Level table for the coef Huffman.
	std::unique_ptr<uint16_t[]> _coefHuffmanIntTable[2];   ///< Int table for the coef Huffman.

	// Noise
	float _noiseMult;                 ///< Noise multiplier.
	float _noiseTable[kNoiseTabSize]; ///< Noise table.
	int   _noiseIndex;

	std::unique_ptr<Common::Huffman> _hgainHuffman; ///< Perceptual noise Huffman code.

	// Exponents
	int   _exponentsBSize[kChannelsMax];
	float _exponents[kChannelsMax][kBlockSizeMax];
	float _maxExponent[kChannelsMax];

	std::unique_ptr<Common::Huffman> _expHuffman; ///< Exponents Huffman code.

	// Coded values in high bands
	bool _highBandCoded [kChannelsMax][kHighBandSizeMax];
	int  _highBandValues[kChannelsMax][kHighBandSizeMax];

	// Coefficients
	float _coefs1[kChannelsMax][kBlockSizeMax];
	float _coefs [kChannelsMax][kBlockSizeMax];

	// Line spectral pairs
	float _lspCosTable[kBlockSizeMax];
	float _lspPowETable[256];
	float _lspPowMTable1[(1 << kLSPPowBits)];
	float _lspPowMTable2[(1 << kLSPPowBits)];

	// MDCT
	Common::PtrVector<Common::MDCT> _mdct;  ///< MDCT contexts.
	std::vector<const float *> _mdctWindow; ///< MDCT window functions.

	/** Overhang from the last superframe. */
	byte _lastSuperframe[kSuperframeSizeMax + 4];
	int  _lastSuperframeLen; ///< Size of the overhang data.
	int  _lastBitoffset;     ///< Bit position within the overhang.

	// Output
	float _output[kBlockSizeMax * 2];
	float _frameOut[kChannelsMax][kBlockSizeMax * 2];

	// Backing stream for PacketizedAudioStream
	std::unique_ptr<QueuingAudioStream> _audStream;

	// Init helpers

	void init(Common::SeekableReadStream *extraData);

	uint16_t getFlags(Common::SeekableReadStream *extraData);
	void evalFlags(uint16_t flags, Common::SeekableReadStream *extraData);
	int getFrameBitLength();
	int getBlockSizeCount(uint16_t flags);
	uint32_t getNormalizedSampleRate();
	bool useNoiseCoding(float &highFreq, float &bps);
	void evalMDCTScales(float highFreq);
	void initNoise();
	void initCoefHuffman(float bps);
	void initMDCT();
	void initExponents();

	Common::Huffman *initCoefHuffman(std::unique_ptr<uint16_t[]> &runTable,
	                                 std::unique_ptr<float[]>  &levelTable,
	                                 std::unique_ptr<uint16_t[]> &intTable,
	                                 const WMACoefHuffmanParam &params);
	void initLSPToCurve();

	// Decoding

	Common::SeekableReadStream *decodeSuperFrame(Common::SeekableReadStream &data);
	bool decodeFrame(Common::BitStream &bits, int16_t *outputData);
	int decodeBlock(Common::BitStream &bits);
	AudioStream *decodeFrame(Common::SeekableReadStream &data);

	// Decoding helpers

	bool evalBlockLength(Common::BitStream &bits);
	bool decodeChannels(Common::BitStream &bits, int bSize, bool msStereo, bool *hasChannel);
	bool calculateIMDCT(int bSize, bool msStereo, bool *hasChannel);

	void calculateCoefCount(int *coefCount, int bSize) const;
	bool decodeNoise(Common::BitStream &bits, int bSize, bool *hasChannel, int *coefCount);
	bool decodeExponents(Common::BitStream &bits, int bSize, bool *hasChannel);
	bool decodeSpectralCoef(Common::BitStream &bits, bool msStereo, bool *hasChannel,
	                        int *coefCount, int coefBitCount);
	float getNormalizedMDCTLength() const;
	void calculateMDCTCoefficients(int bSize, bool *hasChannel,
	                               int *coefCount, int totalGain, float mdctNorm);

	bool decodeExpHuffman(Common::BitStream &bits, int ch);
	bool decodeExpLSP(Common::BitStream &bits, int ch);
	bool decodeRunLevel(Common::BitStream &bits, const Common::Huffman &huffman,
		const float *levelTable, const uint16_t *runTable, int version, float *ptr,
		int offset, int numCoefs, int blockLen, int frameLenBits, int coefNbBits);

	void lspToCurve(float *out, float *val_max_ptr, int n, float *lsp);

	void window(float *out) const;

	float pow_m1_4(float x) const;

	static int readTotalGain(Common::BitStream &bits);
	static int totalGainToBits(int totalGain);
	static uint32_t getLargeVal(Common::BitStream &bits);
};


WMACodec::WMACodec(int version, uint32_t sampleRate, uint8_t channels,
		uint32_t bitRate, uint32_t blockAlign, Common::SeekableReadStream *extraData) :
	_version(version), _sampleRate(sampleRate), _channels(channels),
	_bitRate(bitRate), _blockAlign(blockAlign), _audioFlags(0),
	_resetBlockLengths(true), _curFrame(0), _frameLen(0), _frameLenBits(0),
	_blockSizeCount(0), _framePos(0), _curBlock(0), _blockLen(0), _blockLenBits(0),
	_nextBlockLenBits(0), _prevBlockLenBits(0), _byteOffsetBits(0),
	_lastSuperframeLen(0), _lastBitoffset(0) {

	for (int i = 0; i < 2; i++)
		_coefHuffmanParam[i] = 0;

	if ((_version != 1) && (_version != 2))
		throw Common::Exception("WMACodec::init(): Unsupported WMA version %d", _version);

	if ((_sampleRate == 0) || (_sampleRate > 50000))
		throw Common::Exception("WMACodec::init(): Invalid sample rate %d", _sampleRate);
	if ((_channels == 0) || (_channels > kChannelsMax))
		throw Common::Exception("WMACodec::init(): Unsupported number of channels %d",
		                        _channels);

	_audioFlags = FLAG_16BITS;

#ifdef PHAETHON_LITTLE_ENDIAN
	_audioFlags |= FLAG_LITTLE_ENDIAN;
#endif

	init(extraData);

	_audStream.reset(makeQueuingAudioStream(getRate(), getChannels()));
}

WMACodec::~WMACodec() {
}

void WMACodec::init(Common::SeekableReadStream *extraData) {
	// Flags
	uint16_t flags = getFlags(extraData);
	evalFlags(flags, extraData);

	// Frame length
	_frameLenBits = getFrameBitLength();
	_frameLen     = 1 << _frameLenBits;

	// Number of MDCT block sizes
	_blockSizeCount = getBlockSizeCount(flags);

	float bps = ((float) _bitRate) / ((float) (_channels * _sampleRate));

	_byteOffsetBits = Common::intLog2((int) (bps * _frameLen / 8.0 + 0.05)) + 2;

	// Compute high frequency value and choose if noise coding should be activated
	float highFreq;
	_useNoiseCoding = useNoiseCoding(highFreq, bps);

	// Compute the scale factor band sizes for each MDCT block size
	evalMDCTScales(highFreq);

	// Init the noise generator
	initNoise();

	// Init the coefficient Huffman codes
	initCoefHuffman(bps);

	// Init MDCTs
	initMDCT();

	// Init exponent codes
	initExponents();

	// Clear the sample output buffers
	std::memset(_output  , 0, sizeof(_output));
	std::memset(_frameOut, 0, sizeof(_frameOut));
}

uint16_t WMACodec::getFlags(Common::SeekableReadStream *extraData) {
	if ((_version == 1) && extraData && (extraData->size() >= 4)) {
		extraData->seek(2);
		return extraData->readUint16LE();
	}

	if ((_version == 2) && extraData && (extraData->size() >= 6)) {
		extraData->seek(4);
		return extraData->readUint16LE();
	}

	return 0;
}

void WMACodec::evalFlags(uint16_t flags, Common::SeekableReadStream *extraData) {
	_useExpHuffman       = (flags & 0x0001) != 0;
	_useBitReservoir     = (flags & 0x0002) != 0;
	_useVariableBlockLen = (flags & 0x0004) != 0;

	if ((_version == 2) && extraData && (extraData->size() >= 8)) {
		extraData->seek(4);
		if ((extraData->readUint16LE() == 0x000D) && _useVariableBlockLen) {
			// Apparently, this fixes ffmpeg "issue1503"

			_useVariableBlockLen = false;
		}
	}
}

int WMACodec::getFrameBitLength() {
	if (_sampleRate <= 16000)
		return 9;

	if ((_sampleRate <= 22050) || (_sampleRate <= 32000 && _version == 1))
		return 10;

	if (_sampleRate <= 48000)
		return 11;

	if (_sampleRate <= 96000)
		return 12;

	return 13;
}

int WMACodec::getBlockSizeCount(uint16_t flags) {
	if (!_useVariableBlockLen)
		return 1;

	int count = ((flags >> 3) & 3) + 1;

	if ((_bitRate / _channels) >= 32000)
		count += 2;

	const int maxCount = _frameLenBits - kBlockBitsMin;

	return MIN(count, maxCount) + 1;
}

uint32_t WMACodec::getNormalizedSampleRate() {
	// Sample rates are only normalized in WMAv2
	if (_version != 2)
		return _sampleRate;

	if (_sampleRate>= 44100)
		return 44100;

	if (_sampleRate >= 22050)
		return 22050;

	if (_sampleRate >= 16000)
		return 16000;

	if (_sampleRate >= 11025)
		return 11025;

	if (_sampleRate >=  8000)
		return 8000;

	return _sampleRate;
}

bool WMACodec::useNoiseCoding(float &highFreq, float &bps) {
	highFreq = _sampleRate * 0.5f;

	uint32_t rateNormalized = getNormalizedSampleRate();

	float bpsOrig = bps;
	if (_channels == 2)
		bps = bpsOrig * 1.6f;

	if (rateNormalized == 44100) {
		if (bps >= 0.61f)
			return false;

		highFreq = highFreq * 0.4f;
		return true;
	}

	if (rateNormalized == 22050) {
		if (bps >= 1.16f)
			return false;

		if (bps >= 0.72f)
			highFreq = highFreq * 0.7f;
		else
			highFreq = highFreq * 0.6f;

		return true;
	}

	if (rateNormalized == 16000) {
		if (bpsOrig > 0.5f)
			highFreq = highFreq * 0.5f;
		else
			highFreq = highFreq * 0.3f;

		return true;
	}

	if (rateNormalized == 11025) {
		highFreq = highFreq * 0.7f;
		return true;
	}

	if (rateNormalized == 8000) {
		if (bpsOrig > 0.75f)
			return false;

		if (bpsOrig <= 0.625f)
			highFreq = highFreq * 0.5f;
		else
			highFreq = highFreq * 0.65f;

		return true;
	}


	if (bpsOrig >= 0.8f)
		highFreq = highFreq * 0.75f;
	else if (bpsOrig >= 0.6f)
		highFreq = highFreq * 0.6f;
	else
		highFreq = highFreq * 0.5f;

	return true;
}

void WMACodec::evalMDCTScales(float highFreq) {
	if (_version == 1)
		_coefsStart = 3;
	else
		_coefsStart = 0;

	for (int k = 0; k < _blockSizeCount; k++) {
		int blockLen = _frameLen >> k;

		if (_version == 1) {
			int i, lpos = 0;

			for (i = 0; i < 25; i++) {
				int a   = wmaCriticalFreqs[i];
				int b   = _sampleRate;
				int pos = ((blockLen * 2 * a) + (b >> 1)) / b;

				if (pos > blockLen)
					pos = blockLen;

				_exponentBands[0][i] = pos - lpos;
				if (pos >= blockLen) {
					i++;
					break;
				}
				lpos = pos;
			}

			_exponentSizes[0] = i;

		} else {
			// Hardcoded tables
			const uint8_t *table = 0;

			int t = _frameLenBits - kBlockBitsMin - k;
			if (t < 3) {
				if (_sampleRate >= 44100)
					table = exponentBand44100[t];
				else if (_sampleRate >= 32000)
					table = exponentBand32000[t];
				else if (_sampleRate >= 22050)
					table = exponentBand22050[t];
			}

			if (table) {
				int n = *table++;

				for (int i = 0; i < n; i++)
					_exponentBands[k][i] = table[i];

				_exponentSizes[k] = n;

			} else {
				int j = 0, lpos = 0;

				for (int i = 0; i < 25; i++) {
					int a   = wmaCriticalFreqs[i];
					int b   = _sampleRate;
					int pos = ((blockLen * 2 * a) + (b << 1)) / (4 * b);

					pos <<= 2;
					if (pos > blockLen)
						pos = blockLen;

					if (pos > lpos)
						_exponentBands[k][j++] = pos - lpos;

					if (pos >= blockLen)
						break;

					lpos = pos;
				}

				_exponentSizes[k] = j;
			}

		}

		// Max number of coefs
		_coefsEnd[k] = (_frameLen - ((_frameLen * 9) / 100)) >> k;

		// High freq computation
		_highBandStart[k] = (int)((blockLen * 2 * highFreq) / _sampleRate + 0.5f);

		int n   = _exponentSizes[k];
		int j   = 0;
		int pos = 0;

		for (int i = 0; i < n; i++) {
			int start, end;

			start = pos;
			pos  += _exponentBands[k][i];
			end   = pos;

			if (start < _highBandStart[k])
				start = _highBandStart[k];

			if (end > _coefsEnd[k])
				end = _coefsEnd[k];

			if (end > start)
				_exponentHighBands[k][j++] = end - start;

		}

		_exponentHighSizes[k] = j;
	}
}

void WMACodec::initNoise() {
	if (!_useNoiseCoding)
		return;

	_noiseMult  = _useExpHuffman ? 0.02f : 0.04f;
	_noiseIndex = 0;

	uint  seed = 1;
	float norm = (1.0f / (float)(1LL << 31)) * sqrt(3.0) * _noiseMult;

	for (int i = 0; i < kNoiseTabSize; i++) {
		seed = seed * 314159 + 1;

		_noiseTable[i] = (float)((int)seed) * norm;
	}

	_hgainHuffman = std::make_unique<Common::Huffman>(0, ARRAYSIZE(hgainHuffCodes), hgainHuffCodes, hgainHuffBits);
}

void WMACodec::initCoefHuffman(float bps) {
	// Choose the parameter table
	int coefHuffTable = 2;
	if (_sampleRate >= 32000) {
		if (bps < 0.72f) {
			coefHuffTable = 0;
		} else if (bps < 1.16f) {
			coefHuffTable = 1;
		}
	}

	_coefHuffmanParam[0] = &coefHuffmanParam[coefHuffTable * 2    ];
	_coefHuffmanParam[1] = &coefHuffmanParam[coefHuffTable * 2 + 1];

	_coefHuffman[0].reset(initCoefHuffman(_coefHuffmanRunTable[0], _coefHuffmanLevelTable[0],
	                                      _coefHuffmanIntTable[0], *_coefHuffmanParam[0]));
	_coefHuffman[1].reset(initCoefHuffman(_coefHuffmanRunTable[1], _coefHuffmanLevelTable[1],
	                                      _coefHuffmanIntTable[1], *_coefHuffmanParam[1]));
}

void WMACodec::initMDCT() {
	_mdct.reserve(_blockSizeCount);
	for (int i = 0; i < _blockSizeCount; i++)
		_mdct.push_back(new Common::MDCT(_frameLenBits - i + 1, true, 1.0f));

	// Init MDCT windows (simple sine window)
	_mdctWindow.reserve(_blockSizeCount);
	for (int i = 0; i < _blockSizeCount; i++)
		_mdctWindow.push_back(Common::getSineWindow(_frameLenBits - i));
}

void WMACodec::initExponents() {
	if (_useExpHuffman)
		_expHuffman = std::make_unique<Common::Huffman>(0, ARRAYSIZE(scaleHuffCodes), scaleHuffCodes, scaleHuffBits);
	else
		initLSPToCurve();
}

Common::Huffman *WMACodec::initCoefHuffman(std::unique_ptr<uint16_t[]> &runTable,
                                           std::unique_ptr<float[]>  &levelTable,
                                           std::unique_ptr<uint16_t[]> &intTable,
                                           const WMACoefHuffmanParam &params) {

	Common::Huffman *huffman =
		new Common::Huffman(0, params.n, params.huffCodes, params.huffBits);

	runTable   = std::make_unique<uint16_t[]>(params.n);
	levelTable = std::make_unique< float[]>(params.n);
	intTable   = std::make_unique<uint16_t[]>(params.n);

	std::unique_ptr<uint16_t[]> iLevelTable = std::make_unique<uint16_t[]>(params.n);

	int i = 2;
	int level = 1;
	int k = 0;

	while (i < params.n) {
		intTable[k] = i;

		int l = params.levels[k++];

		for (int j = 0; j < l; j++) {
			runTable   [i] = j;
			iLevelTable[i] = level;
			levelTable [i] = level;

			i++;
		}

		level++;
	}

	return huffman;
}

void WMACodec::initLSPToCurve() {
	float wdel = M_PI / _frameLen;

	for (int i = 0; i < _frameLen; i++)
		_lspCosTable[i] = 2.0f * cosf(wdel * i);

	// Tables for x^-0.25 computation
	for (int i = 0; i < 256; i++) {
		int e = i - 126;

		_lspPowETable[i] = powf(2.0f, e * -0.25f);
	}

	// NOTE: These two tables are needed to avoid two operations in pow_m1_4
	float b = 1.0f;
	for (int i = (1 << kLSPPowBits) - 1; i >= 0; i--) {
		int   m = (1 << kLSPPowBits) + i;
		float a = (float) m * (0.5f / (1 << kLSPPowBits));

		a = pow(a, -0.25f);

		_lspPowMTable1[i] = 2 * a - b;
		_lspPowMTable2[i] = b - a;

		b = a;
	}
}

AudioStream *WMACodec::decodeFrame(Common::SeekableReadStream &data) {
	Common::SeekableReadStream *stream = decodeSuperFrame(data);
	if (!stream)
		return 0;

	return makePCMStream(stream, _sampleRate, _audioFlags, _channels, true);
}

Common::SeekableReadStream *WMACodec::decodeSuperFrame(Common::SeekableReadStream &data) {
	uint32_t size = data.size();
	if (size < _blockAlign) {
		warning("WMACodec::decodeSuperFrame(): size < _blockAlign");
		return 0;
	}

	if (_blockAlign)
		size = _blockAlign;

	Common::BitStream8MSB bits(data);

	int outputDataSize = 0;
	std::unique_ptr<int16_t[]> outputData;

	_curFrame = 0;

	if (_useBitReservoir) {
		// This superframe consists of more than just one frame

		bits.skip(4); // Super frame index

		// Number of frames in this superframe
		int newFrameCount = bits.getBits(4) - 1;
		if (newFrameCount < 0) {
			warning("WMACodec::decodeSuperFrame(): newFrameCount == %d", newFrameCount);

			_resetBlockLengths = true;
			_lastSuperframeLen = 0;
			_lastBitoffset     = 0;

			return 0;
		}

		// Number of frames in this superframe + overhang from the last superframe
		int frameCount = newFrameCount;
		if (_lastSuperframeLen > 0)
			frameCount++;

		// PCM output data
		outputDataSize = frameCount * _channels * _frameLen;
		outputData = std::make_unique<int16_t[]>(outputDataSize);

		std::memset(outputData.get(), 0, outputDataSize * 2);

		// Number of bits data that completes the last superframe's overhang.
		int bitOffset = bits.getBits(_byteOffsetBits + 3);

		if (_lastSuperframeLen > 0) {
			// We have overhang data from the last superframe. Paste the
			// complementary data from this superframe at the end and
			// decode it as another frame.

			byte *lastSuperframeEnd = _lastSuperframe + _lastSuperframeLen;

			while (bitOffset > 7) { // Full bytes
				*lastSuperframeEnd++ = bits.getBits(8);

				bitOffset          -= 8;
				_lastSuperframeLen += 1;
			}

			if (bitOffset > 0) { // Remaining bits
				*lastSuperframeEnd++ = bits.getBits(bitOffset) << (8 - bitOffset);

				bitOffset           = 0;
				_lastSuperframeLen += 1;
			}

			Common::MemoryReadStream lastSuperframe(_lastSuperframe, _lastSuperframeLen);
			Common::BitStream8MSB lastBits(lastSuperframe);

			lastBits.skip(_lastBitoffset);

			decodeFrame(lastBits, outputData.get());

			_curFrame++;
		}

		// Skip any complementary data we haven't used
		bits.skip(bitOffset);

		// New superframe = New block lengths
		_resetBlockLengths = true;

		// Decode the frames
		for (int i = 0; i < newFrameCount; i++, _curFrame++)
			if (!decodeFrame(bits, outputData.get()))
				return 0;

		// Check if we've got new overhang data
		int remainingBits = bits.size() - bits.pos();
		if (remainingBits > 0) {
			// We do: Save it

			_lastSuperframeLen = remainingBits >> 3;
			_lastBitoffset     = 8 - (remainingBits - (_lastSuperframeLen << 3));

			if (_lastBitoffset > 0)
				_lastSuperframeLen++;

			data.seek(data.size() - _lastSuperframeLen);
			data.read(_lastSuperframe, _lastSuperframeLen);
		} else {
			// We don't

			_lastSuperframeLen = 0;
			_lastBitoffset     = 0;
		}

	} else {
		// This superframe has only one frame

		// PCM output data
		outputDataSize = _channels * _frameLen;
		outputData = std::make_unique<int16_t[]>(outputDataSize);

		std::memset(outputData.get(), 0, outputDataSize * 2);

		// Decode the frame
		if (!decodeFrame(bits, outputData.get()))
			return 0;
	}

	// And return our PCM output data as a stream, if available

	if (!outputData)
		return 0;

	// TODO: This might be a problem alignment-wise?
	return new Common::MemoryReadStream(reinterpret_cast<byte *>(outputData.release()), outputDataSize * 2, true);
}

bool WMACodec::decodeFrame(Common::BitStream &bits, int16_t *outputData) {
	_framePos = 0;
	_curBlock = 0;

	// Decode all blocks
	int finished = 0;
	while (finished == 0)
		finished = decodeBlock(bits);

	// Check for error
	if (finished < 0)
		return false;

	// Convert output into interleaved PCM data

	const float *floatOut[kChannelsMax];
	for (int i = 0; i < kChannelsMax; i++)
		floatOut[i] = _frameOut[i];

	int16_t *pcmOut = outputData + _curFrame * _channels * _frameLen;

	floatToInt16Interleave(pcmOut, floatOut, _frameLen, _channels);

	// Prepare for the next frame
	for (int i = 0; i < _channels; i++)
		memmove(&_frameOut[i][0], &_frameOut[i][_frameLen], _frameLen * sizeof(float));

	return true;
}

int WMACodec::decodeBlock(Common::BitStream &bits) {
	// Computer new block length
	if (!evalBlockLength(bits))
		return -1;

	// Block size

	int bSize = _frameLenBits - _blockLenBits;
	assert((bSize >= 0) && (bSize < _blockSizeCount));

	// MS Stereo?

	bool msStereo = false;
	if (_channels == 2)
		msStereo = bits.getBit() != 0;

	// Which channels are encoded?

	bool hasChannels = false;
	bool hasChannel[kChannelsMax];
	for (int i = 0; i < kChannelsMax; i++)
		hasChannel[i] = false;

	for (int i = 0; i < _channels; i++) {
		hasChannel[i] = bits.getBit() != 0;
		if (hasChannel[i])
			hasChannels = true;
	}

	// Decode channels

	if (hasChannels)
		if (!decodeChannels(bits, bSize, msStereo, hasChannel))
			return -1;

	// Calculate IMDCTs

	if (!calculateIMDCT(bSize, msStereo, hasChannel))
		return -1;

	// Update block number

	_curBlock += 1;
	_framePos += _blockLen;

	// Finished
	if (_framePos >= _frameLen)
		return 1;

	// Need more blocks
	return 0;
}

bool WMACodec::decodeChannels(Common::BitStream &bits, int bSize,
                              bool msStereo, bool *hasChannel) {

	int totalGain    = readTotalGain(bits);
	int coefBitCount = totalGainToBits(totalGain);

	int coefCount[kChannelsMax];
	calculateCoefCount(coefCount, bSize);

	if (!decodeNoise(bits, bSize, hasChannel, coefCount))
		return false;

	if (!decodeExponents(bits, bSize, hasChannel))
		return false;

	if (!decodeSpectralCoef(bits, msStereo, hasChannel, coefCount, coefBitCount))
		return false;

	float mdctNorm = getNormalizedMDCTLength();

	calculateMDCTCoefficients(bSize, hasChannel, coefCount, totalGain, mdctNorm);

	if (msStereo && hasChannel[1]) {
		// Nominal case for ms stereo: we do it before MDCT
		// No need to optimize this case because it should almost never happen

		if (!hasChannel[0]) {
			std::memset(_coefs[0], 0, sizeof(float) * _blockLen);
			hasChannel[0] = true;
		}

		butterflyFloats(_coefs[0], _coefs[1], _blockLen);
	}

	return true;
}

bool WMACodec::calculateIMDCT(int bSize, bool msStereo, bool *hasChannel) {
	Common::MDCT &mdct = *_mdct[bSize];

	for (int i = 0; i < _channels; i++) {
		int n4 = _blockLen / 2;

		if (hasChannel[i])
			mdct.calcIMDCT(_output, _coefs[i]);
		else if (!(msStereo && (i == 1)))
			std::memset(_output, 0, sizeof(_output));

		// Multiply by the window and add in the frame
		int index = (_frameLen / 2) + _framePos - n4;
		window(&_frameOut[i][index]);
	}

	return true;
}

bool WMACodec::evalBlockLength(Common::BitStream &bits) {
	if (_useVariableBlockLen) {
		// Variable block lengths

		int n = Common::intLog2(_blockSizeCount - 1) + 1;

		if (_resetBlockLengths) {
			// Completely new block lengths

			_resetBlockLengths = false;

			const int prev     = bits.getBits(n);
			const int prevBits = _frameLenBits - prev;
			if (prev >= _blockSizeCount) {
				warning("WMACodec::evalBlockLength(): _prevBlockLenBits %d out of range", prevBits);
				return false;
			}

			_prevBlockLenBits = prevBits;

			const int cur     = bits.getBits(n);
			const int curBits = _frameLenBits - cur;
			if (cur >= _blockSizeCount) {
				warning("WMACodec::evalBlockLength(): _blockLenBits %d out of range", curBits);
				return false;
			}

			_blockLenBits = curBits;

		} else {
			// Update block lengths

			_prevBlockLenBits = _blockLenBits;
			_blockLenBits     = _nextBlockLenBits;
		}

		const int next     = bits.getBits(n);
		const int nextBits = _frameLenBits - next;
		if (next >= _blockSizeCount) {
			warning("WMACodec::evalBlockLength(): _nextBlockLenBits %d out of range", nextBits);
			return false;
		}

		_nextBlockLenBits = nextBits;

	} else {
		// Fixed block length

		_nextBlockLenBits = _frameLenBits;
		_prevBlockLenBits = _frameLenBits;
		_blockLenBits     = _frameLenBits;
	}

	// Sanity checks

	if (_frameLenBits - _blockLenBits >= _blockSizeCount) {
		warning("WMACodec::evalBlockLength(): _blockLenBits not initialized to a valid value");
		return false;
	}

	_blockLen = 1 << _blockLenBits;
	if ((_framePos + _blockLen) > _frameLen) {
		warning("WMACodec::evalBlockLength(): frame length overflow");
		return false;
	}

	return true;
}

void WMACodec::calculateCoefCount(int *coefCount, int bSize) const {
	const int coefN = _coefsEnd[bSize] - _coefsStart;

	for (int i = 0; i < _channels; i++)
		coefCount[i] = coefN;
}

bool WMACodec::decodeNoise(Common::BitStream &bits, int bSize,
                           bool *hasChannel, int *coefCount) {
	if (!_useNoiseCoding)
		return true;

	for (int i = 0; i < _channels; i++) {
		if (!hasChannel[i])
			continue;

		const int n = _exponentHighSizes[bSize];
		for (int j = 0; j < n; j++) {
			bool a = bits.getBit() != 0;
			_highBandCoded[i][j] = a;

			// With noise coding, the coefficients are not transmitted
			if (a)
				coefCount[i] -= _exponentHighBands[bSize][j];
		}
	}

	for (int i = 0; i < _channels; i++) {
		if (!hasChannel[i])
			continue;

		const int n   = _exponentHighSizes[bSize];
		      int val = (int) 0x80000000;

		for (int j = 0; j < n; j++) {
			if (!_highBandCoded[i][j])
				continue;

			if (val != (int) 0x80000000) {
				int code = _hgainHuffman->getSymbol(bits);
				if (code < 0) {
					warning("WMACodec::decodeNoise(): HGain Huffman invalid");
					return false;
				}

				val += code - 18;

			} else
				val = bits.getBits(7) - 19;

			_highBandValues[i][j] = val;

		}
	}

	return true;
}

bool WMACodec::decodeExponents(Common::BitStream &bits, int bSize, bool *hasChannel) {
	// Exponents can be reused in short blocks
	if (!((_blockLenBits == _frameLenBits) || bits.getBit()))
		return true;

	for (int i = 0; i < _channels; i++) {
		if (!hasChannel[i])
			continue;

		if (_useExpHuffman) {
			if (!decodeExpHuffman(bits, i))
				return false;
		} else {
			if (!decodeExpLSP(bits, i))
				return false;
		}

		_exponentsBSize[i] = bSize;
	}

	return true;
}

bool WMACodec::decodeSpectralCoef(Common::BitStream &bits, bool msStereo, bool *hasChannel,
                                  int *coefCount, int coefBitCount) {
	// Simple RLE encoding

	for (int i = 0; i < _channels; i++) {
		if (hasChannel[i]) {
			// Special Huffman tables are used for MS stereo
			// because there is potentially less energy there.
			const int tindex = ((i == 1) && msStereo);

			float *ptr = &_coefs1[i][0];
			std::memset(ptr, 0, _blockLen * sizeof(float));

			if (!decodeRunLevel(bits, *_coefHuffman[tindex],
			                    _coefHuffmanLevelTable[tindex].get(), _coefHuffmanRunTable[tindex].get(),
			                    0, ptr, 0, coefCount[i], _blockLen, _frameLenBits, coefBitCount))
				return false;
		}

		if ((_version == 1) && (_channels >= 2))
			bits.skip(-((ptrdiff_t)(bits.pos() & 7)));
	}

	return true;
}

float WMACodec::getNormalizedMDCTLength() const {
	const int n4 = _blockLen / 2;

	float mdctNorm = 1.0f / (float) n4;
	if (_version == 1)
		mdctNorm *= sqrt((float) n4);

	return mdctNorm;
}

void WMACodec::calculateMDCTCoefficients(int bSize, bool *hasChannel,
                                        int *coefCount, int totalGain, float mdctNorm) {

	for (int i = 0; i < _channels; i++) {
		if (!hasChannel[i])
			continue;

		      float *coefs     = _coefs[i];
		const float *coefs1    = _coefs1[i];
		const float *exponents = _exponents[i];

		const int eSize = _exponentsBSize[i];

		const float mult = (pow(10, totalGain * 0.05f) / _maxExponent[i]) * mdctNorm;

		if (_useNoiseCoding) {

			// Very low freqs: noise
			for (int j = 0; j < _coefsStart; j++) {
				*coefs++ = _noiseTable[_noiseIndex] * exponents[(j << bSize) >> eSize] * mult;

				_noiseIndex = (_noiseIndex + 1) & (kNoiseTabSize - 1);
			}

			// Compute power of high bands
			float expPower[kHighBandSizeMax] = {
				1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f
			};

			const int n1 = _exponentHighSizes[bSize];
			exponents = _exponents[i] + ((_highBandStart[bSize] << bSize) >> eSize);

			int lastHighBand = 0;
			for (int k = 0; k < n1; k++) {
				const int n = _exponentHighBands[_frameLenBits - _blockLenBits][k];

				if (_highBandCoded[i][k]) {
					float e2 = 0;

					for (int j = 0; j < n; j++) {
						const float v = exponents[(j << bSize) >> eSize];

						e2 += v * v;
					}

					expPower[k] = e2 / n;
					lastHighBand = k;
				}

				exponents += (n << bSize) >> eSize;
			}

			// Main freqs and high freqs
			exponents = _exponents[i] + ((_coefsStart << bSize) >> eSize);

			for (int k = -1; k < n1; k++) {

				int n;
				if (k < 0)
					n = _highBandStart[bSize] - _coefsStart;
				else
					n = _exponentHighBands[_frameLenBits - _blockLenBits][k];

				if (k >= 0 && _highBandCoded[i][k]) {
					// Use noise with specified power

					float mult1 = sqrt(expPower[k] / expPower[lastHighBand]);

					mult1 *= pow(10, _highBandValues[i][k] * 0.05f);
					mult1 /= _maxExponent[i] * _noiseMult;
					mult1 *= mdctNorm;

					for (int j = 0; j < n; j++) {
						float noise = _noiseTable[_noiseIndex];

						_noiseIndex = (_noiseIndex + 1) & (kNoiseTabSize - 1);
						*coefs++    = noise * exponents[(j << bSize) >> eSize] * mult1;
					}

					exponents += (n << bSize) >> eSize;

				} else {
					// Coded values + small noise

					for (int j = 0; j < n; j++) {
						float noise = _noiseTable[_noiseIndex];

						_noiseIndex = (_noiseIndex + 1) & (kNoiseTabSize - 1);
						*coefs++    = ((*coefs1++) + noise) * exponents[(j << bSize) >> eSize] * mult;
					}

					exponents += (n << bSize) >> eSize;
				}

			}

			// Very high freqs: Noise
			const int   n     = _blockLen - _coefsEnd[bSize];
			const float mult1 = mult * exponents[(-(1 << bSize)) >> eSize];

			for (int j = 0; j < n; j++) {
				*coefs++    = _noiseTable[_noiseIndex] * mult1;
				_noiseIndex = (_noiseIndex + 1) & (kNoiseTabSize - 1);
			}

		} else {

			for (int j = 0; j < _coefsStart; j++)
				*coefs++ = 0.0f;

			for (int j = 0;j < coefCount[i]; j++) {
				*coefs = coefs1[j] * exponents[(j << bSize) >> eSize] * mult;
				coefs++;
			}

			int n = _blockLen - _coefsEnd[bSize];
			for (int j = 0; j < n; j++)
				*coefs++ = 0.0f;

		}

	}

}

static const float powTab[] = {
	1.7782794100389e-04f, 2.0535250264571e-04f,
	2.3713737056617e-04f, 2.7384196342644e-04f,
	3.1622776601684e-04f, 3.6517412725484e-04f,
	4.2169650342858e-04f, 4.8696752516586e-04f,
	5.6234132519035e-04f, 6.4938163157621e-04f,
	7.4989420933246e-04f, 8.6596432336006e-04f,
	1.0000000000000e-03f, 1.1547819846895e-03f,
	1.3335214321633e-03f, 1.5399265260595e-03f,
	1.7782794100389e-03f, 2.0535250264571e-03f,
	2.3713737056617e-03f, 2.7384196342644e-03f,
	3.1622776601684e-03f, 3.6517412725484e-03f,
	4.2169650342858e-03f, 4.8696752516586e-03f,
	5.6234132519035e-03f, 6.4938163157621e-03f,
	7.4989420933246e-03f, 8.6596432336006e-03f,
	1.0000000000000e-02f, 1.1547819846895e-02f,
	1.3335214321633e-02f, 1.5399265260595e-02f,
	1.7782794100389e-02f, 2.0535250264571e-02f,
	2.3713737056617e-02f, 2.7384196342644e-02f,
	3.1622776601684e-02f, 3.6517412725484e-02f,
	4.2169650342858e-02f, 4.8696752516586e-02f,
	5.6234132519035e-02f, 6.4938163157621e-02f,
	7.4989420933246e-02f, 8.6596432336007e-02f,
	1.0000000000000e-01f, 1.1547819846895e-01f,
	1.3335214321633e-01f, 1.5399265260595e-01f,
	1.7782794100389e-01f, 2.0535250264571e-01f,
	2.3713737056617e-01f, 2.7384196342644e-01f,
	3.1622776601684e-01f, 3.6517412725484e-01f,
	4.2169650342858e-01f, 4.8696752516586e-01f,
	5.6234132519035e-01f, 6.4938163157621e-01f,
	7.4989420933246e-01f, 8.6596432336007e-01f,
	1.0000000000000e+00f, 1.1547819846895e+00f,
	1.3335214321633e+00f, 1.5399265260595e+00f,
	1.7782794100389e+00f, 2.0535250264571e+00f,
	2.3713737056617e+00f, 2.7384196342644e+00f,
	3.1622776601684e+00f, 3.6517412725484e+00f,
	4.2169650342858e+00f, 4.8696752516586e+00f,
	5.6234132519035e+00f, 6.4938163157621e+00f,
	7.4989420933246e+00f, 8.6596432336007e+00f,
	1.0000000000000e+01f, 1.1547819846895e+01f,
	1.3335214321633e+01f, 1.5399265260595e+01f,
	1.7782794100389e+01f, 2.0535250264571e+01f,
	2.3713737056617e+01f, 2.7384196342644e+01f,
	3.1622776601684e+01f, 3.6517412725484e+01f,
	4.2169650342858e+01f, 4.8696752516586e+01f,
	5.6234132519035e+01f, 6.4938163157621e+01f,
	7.4989420933246e+01f, 8.6596432336007e+01f,
	1.0000000000000e+02f, 1.1547819846895e+02f,
	1.3335214321633e+02f, 1.5399265260595e+02f,
	1.7782794100389e+02f, 2.0535250264571e+02f,
	2.3713737056617e+02f, 2.7384196342644e+02f,
	3.1622776601684e+02f, 3.6517412725484e+02f,
	4.2169650342858e+02f, 4.8696752516586e+02f,
	5.6234132519035e+02f, 6.4938163157621e+02f,
	7.4989420933246e+02f, 8.6596432336007e+02f,
	1.0000000000000e+03f, 1.1547819846895e+03f,
	1.3335214321633e+03f, 1.5399265260595e+03f,
	1.7782794100389e+03f, 2.0535250264571e+03f,
	2.3713737056617e+03f, 2.7384196342644e+03f,
	3.1622776601684e+03f, 3.6517412725484e+03f,
	4.2169650342858e+03f, 4.8696752516586e+03f,
	5.6234132519035e+03f, 6.4938163157621e+03f,
	7.4989420933246e+03f, 8.6596432336007e+03f,
	1.0000000000000e+04f, 1.1547819846895e+04f,
	1.3335214321633e+04f, 1.5399265260595e+04f,
	1.7782794100389e+04f, 2.0535250264571e+04f,
	2.3713737056617e+04f, 2.7384196342644e+04f,
	3.1622776601684e+04f, 3.6517412725484e+04f,
	4.2169650342858e+04f, 4.8696752516586e+04f,
	5.6234132519035e+04f, 6.4938163157621e+04f,
	7.4989420933246e+04f, 8.6596432336007e+04f,
	1.0000000000000e+05f, 1.1547819846895e+05f,
	1.3335214321633e+05f, 1.5399265260595e+05f,
	1.7782794100389e+05f, 2.0535250264571e+05f,
	2.3713737056617e+05f, 2.7384196342644e+05f,
	3.1622776601684e+05f, 3.6517412725484e+05f,
	4.2169650342858e+05f, 4.8696752516586e+05f,
	5.6234132519035e+05f, 6.4938163157621e+05f,
	7.4989420933246e+05f, 8.6596432336007e+05f,
};

bool WMACodec::decodeExpHuffman(Common::BitStream &bits, int ch) {
	const float  *ptab  = powTab + 60;
	const uint32_t *iptab = reinterpret_cast<const uint32_t *>(ptab);

	const uint16_t *ptr = _exponentBands[_frameLenBits - _blockLenBits];

	uint32_t *q = reinterpret_cast<uint32_t *>(_exponents[ch]);
	uint32_t *qEnd = q + _blockLen;

	float maxScale = 0;

	int lastExp;
	if (_version == 1) {

		lastExp = bits.getBits(5) + 10;

		float   v = ptab[lastExp];
		uint32_t iv = iptab[lastExp];

		maxScale = v;

		int n = *ptr++;

		switch (n & 3) do {
			PHAETHON_FALLTHROUGH;
			case 0: *q++ = iv; PHAETHON_FALLTHROUGH;
			case 3: *q++ = iv; PHAETHON_FALLTHROUGH;
			case 2: *q++ = iv; PHAETHON_FALLTHROUGH;
			case 1: *q++ = iv;
		} while ((n -= 4) > 0);

	} else
		lastExp = 36;

	while (q < qEnd) {
		int code = _expHuffman->getSymbol(bits);
		if (code < 0) {
			warning("WMACodec::decodeExpHuffman(): Exponent invalid");
			return false;
		}

		// NOTE: This offset is the same as MPEG4 AAC!
		lastExp += code - 60;
		if ((unsigned) lastExp + 60 >= ARRAYSIZE(powTab)) {
			warning("WMACodec::decodeExpHuffman(): Exponent out of range: %d", lastExp);
			return false;
		}

		float   v = ptab[lastExp];
		uint32_t iv = iptab[lastExp];

		if (v > maxScale)
			maxScale = v;

		int n = *ptr++;

		switch (n & 3) do {
			PHAETHON_FALLTHROUGH;
			case 0: *q++ = iv; PHAETHON_FALLTHROUGH;
			case 3: *q++ = iv; PHAETHON_FALLTHROUGH;
			case 2: *q++ = iv; PHAETHON_FALLTHROUGH;
			case 1: *q++ = iv;
		} while ((n -= 4) > 0);

	}

	_maxExponent[ch] = maxScale;

	return true;
}

void WMACodec::lspToCurve(float *out, float *val_max_ptr, int n, float *lsp) {
	float val_max = 0;

	for (int i = 0; i < n; i++) {
		float p = 0.5f;
		float q = 0.5f;
		float w = _lspCosTable[i];

		for (int j = 1; j < kLSPCoefCount; j += 2) {
			q *= w - lsp[j - 1];
			p *= w - lsp[j];
		}

		p *= p * (2.0f - w);
		q *= q * (2.0f + w);

		float v = p + q;
		v = pow_m1_4(v);

		if (v > val_max)
			val_max = v;

		out[i] = v;
	}

	*val_max_ptr = val_max;
}

// Decode exponents coded with LSP coefficients (same idea as Vorbis)
bool WMACodec::decodeExpLSP(Common::BitStream &bits, int ch) {
	float lspCoefs[kLSPCoefCount];

	for (int i = 0; i < kLSPCoefCount; i++) {
		int val;

		if (i == 0 || i >= 8)
			val = bits.getBits(3);
		else
			val = bits.getBits(4);

		lspCoefs[i] = lspCodebook[i][val];
	}

	lspToCurve(_exponents[ch], &_maxExponent[ch], _blockLen, lspCoefs);
	return true;
}

bool WMACodec::decodeRunLevel(Common::BitStream &bits, const Common::Huffman &huffman,
	const float *levelTable, const uint16_t *runTable, int version, float *ptr,
	int offset, int numCoefs, int blockLen, int frameLenBits, int coefNbBits) {

	const unsigned int coefMask = blockLen - 1;

	for (; offset < numCoefs; offset++) {
		const int code = huffman.getSymbol(bits);

		if (code > 1) {
			// Normal code

			const float sign = bits.getBit() ? 1.0f : -1.0f;

			offset += runTable[code];

			ptr[offset & coefMask] = levelTable[code] * sign;

		} else if (code == 1) {
			// EOB

			break;

		} else {
			// Escape

			int level;

			if (!version) {

				level   = bits.getBits(coefNbBits);
				// NOTE: This is rather suboptimal. reading blockLenBits would be better
				offset += bits.getBits(frameLenBits);

			} else {
				level = getLargeVal(bits);

				// Escape decode
				if (bits.getBit()) {
					if (bits.getBit()) {
						if (bits.getBit()) {
							warning("WMACodec::decodeRunLevel(): Broken escape sequence");
							return false;
						} else
							offset += bits.getBits(frameLenBits) + 4;
					} else
						offset += bits.getBits(2) + 1;
				}

			}

			const int sign = bits.getBit() - 1;

			ptr[offset & coefMask] = (level ^ sign) - sign;

		}
	}

	// NOTE: EOB can be omitted
	if (offset > numCoefs) {
		warning("WMACodec::decodeRunLevel(): Overflow in spectral RLE, ignoring");
		return true;
	}

	return true;
}

/** Apply MDCT window and add into output.
 *
 *  We ensure that when the windows overlap their squared sum
 *  is always 1 (MDCT reconstruction rule).
 */
void WMACodec::window(float *out) const {
	const float *in = _output;

	// Left part
	if (_blockLenBits <= _prevBlockLenBits) {

		const int bSize = _frameLenBits - _blockLenBits;

		vectorFMulAdd(out, in, _mdctWindow[bSize], out, _blockLen);

	} else {

		const int blockLen = 1 << _prevBlockLenBits;
		const int n = (_blockLen - blockLen) / 2;

		const int bSize = _frameLenBits - _prevBlockLenBits;

		vectorFMulAdd(out + n, in + n, _mdctWindow[bSize], out + n, blockLen);

		std::memcpy(out + n + blockLen, in + n + blockLen, n * sizeof(float));
	}

	out += _blockLen;
	in  += _blockLen;

	// Right part
	if (_blockLenBits <= _nextBlockLenBits) {

		const int bSize = _frameLenBits - _blockLenBits;

		vectorFMulReverse(out, in, _mdctWindow[bSize], _blockLen);

	} else {

		const int blockLen = 1 << _nextBlockLenBits;
		const int n = (_blockLen - blockLen) / 2;

		const int bSize = _frameLenBits - _nextBlockLenBits;

		std::memcpy(out, in, n*sizeof(float));

		vectorFMulReverse(out + n, in + n, _mdctWindow[bSize], blockLen);

		std::memset(out + n + blockLen, 0, n * sizeof(float));
	}
}

float WMACodec::pow_m1_4(float x) const {
	union {
		float f;
		unsigned int v;
	} u, t;

	u.f = x;

	const unsigned int e =  u.v >>  23;
	const unsigned int m = (u.v >> (23 - kLSPPowBits)) & ((1 << kLSPPowBits) - 1);

	// Build interpolation scale: 1 <= t < 2
	t.v = ((u.v << kLSPPowBits) & ((1 << 23) - 1)) | (127 << 23);

	const float a = _lspPowMTable1[m];
	const float b = _lspPowMTable2[m];

	return _lspPowETable[e] * (a + b * t.f);
}

int WMACodec::readTotalGain(Common::BitStream &bits) {
	int totalGain = 1;

	int v = 127;
	while (v == 127) {
		v = bits.getBits(7);

		totalGain += v;
	}

	return totalGain;
}

int WMACodec::totalGainToBits(int totalGain) {
	     if (totalGain < 15) return 13;
	else if (totalGain < 32) return 12;
	else if (totalGain < 40) return 11;
	else if (totalGain < 45) return 10;
	else                     return  9;
}

uint32_t WMACodec::getLargeVal(Common::BitStream &bits) {
	// Consumes up to 34 bits

	int count = 8;
	if (bits.getBit()) {
		count += 8;

		if (bits.getBit()) {
			count += 8;

			if (bits.getBit())
				count += 7;
		}
	}

	return bits.getBits(count);
}

void WMACodec::queuePacket(Common::SeekableReadStream *data) {
	std::unique_ptr<Common::SeekableReadStream> capture(data);
	AudioStream* stream = decodeFrame(*data);
	if (stream)
		_audStream->queueAudioStream(stream);
}

PacketizedAudioStream *makeWMAStream(int version, uint32_t sampleRate, uint8_t channels, uint32_t bitRate, uint32_t blockAlign, Common::SeekableReadStream &extraData) {
	return new WMACodec(version, sampleRate, channels, bitRate, blockAlign, &extraData);
}

} // End of namespace Sound
