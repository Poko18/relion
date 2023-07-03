#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <algorithm>
#include <omp.h>

#include <src/time.h>
#include <src/metadata_table.h>
#include <src/image.h>
#include <src/renderEER.h>

//#define DEBUG_EER

//#define TIMING
#ifdef TIMING
	#define RCTIC(label) (EERtimer.tic(label))
	#define RCTOC(label) (EERtimer.toc(label))

	Timer EERtimer;
	int TIMING_READ_EER = EERtimer.setNew("read EER");
	int TIMING_BUILD_INDEX = EERtimer.setNew("build index");
	int TIMING_UNPACK_RLE = EERtimer.setNew("unpack RLE");
	int TIMING_RENDER_ELECTRONS = EERtimer.setNew("render electrons");
#else
	#define RCTIC(label)
	#define RCTOC(label)
#endif

const char EERRenderer::EER_FOOTER_OK[]  = "ThermoFisherECComprOK000";
const char EERRenderer::EER_FOOTER_ERR[] = "ThermoFisherECComprERR00";
const int EERRenderer::EER_4K= 4096;
const int EERRenderer::EER_2K = 2048;
const unsigned int EERRenderer::EER_LEN_FOOTER = 24;
const uint16_t EERRenderer::TIFF_COMPRESSION_EER8bit = 65000;
const uint16_t EERRenderer::TIFF_COMPRESSION_EER7bit = 65001;
const uint16_t EERRenderer::TIFF_COMPRESSION_EERDetailed = 65002;
const ttag_t EERRenderer::TIFFTAG_EER_RLE_DEPTH = 65007;
const ttag_t EERRenderer::TIFFTAG_EER_SUBPIXEL_H_DEPTH = 65008;
const ttag_t EERRenderer::TIFFTAG_EER_SUBPIXEL_V_DEPTH = 65009;

TIFFErrorHandler EERRenderer::prevTIFFWarningHandler = NULL;

void EERRenderer::TIFFWarningHandler(const char* module, const char* fmt, va_list ap)
{
	// Silence warnings for private tags
	if (strcmp("Unknown field with tag %d (0x%x) encountered", fmt) == 0)
		return;

	if (prevTIFFWarningHandler != NULL)
		prevTIFFWarningHandler(module, fmt, ap);
}

void EERRenderer::silenceTIFFWarnings()
{
	if (prevTIFFWarningHandler == NULL)
	{
		// Thread safety issue:
		// Calling this simultaneously is safe but
		TIFFErrorHandler prev = TIFFSetWarningHandler(EERRenderer::TIFFWarningHandler);

		// we have to make sure prevTIFFWarningHandler does NOT become our own TIFFWarningHandler
		// to avoid an infinite loop.
		if (prev != EERRenderer::TIFFWarningHandler)
			prevTIFFWarningHandler = prev;
	}
}

template <typename T>
void EERRenderer::render4K_to_16K(MultidimArray<T> &image, std::vector<unsigned int> &positions, std::vector<unsigned char> &symbols, int n_electrons)
{
	for (int i = 0; i < n_electrons; i++)
	{
		int x = ((positions[i] & 4095) << 2) | (symbols[i] & 3); // 4095 = 111111111111b, 3 = 00000011b
		int y = ((positions[i] >> 12) << 2) | ((symbols[i] & 12) >> 2); //  4096 = 2^12, 12 = 00001100b
		DIRECT_A2D_ELEM(image, y, x)++;
	}
}

template <typename T>
void EERRenderer::render4K_to_8K(MultidimArray<T> &image, std::vector<unsigned int> &positions, std::vector<unsigned char> &symbols, int n_electrons)
{
	for (int i = 0; i < n_electrons; i++)
	{
		int x = ((positions[i] & 4095) << 1) | ((symbols[i] & 2) >> 1); // 4095 = 111111111111b, 2 = 00000010b
		int y = ((positions[i] >> 12) << 1) | ((symbols[i] & 8) >> 3); //  4096 = 2^12, 8 = 00001000b
		DIRECT_A2D_ELEM(image, y, x)++;
	}
}

template <typename T>
void EERRenderer::render4K_to_4K(MultidimArray<T> &image, std::vector<unsigned int> &positions, std::vector<unsigned char> &symbols, int n_electrons)
{
	for (int i = 0; i < n_electrons; i++)
	{
		int x = positions[i] & 4095; // 4095 = 111111111111b
		int y = positions[i] >> 12; //  4096 = 2^12
		DIRECT_A2D_ELEM(image, y, x)++;
	}
}

template <typename T>
void EERRenderer::render2K_to_4K(MultidimArray<T> &image, std::vector<unsigned int> &positions, std::vector<unsigned char> &symbols, int n_electrons)
{
	for (int i = 0; i < n_electrons; i++)
	{
		int x = ((positions[i] & 2047) << 1) | (symbols[i] & 2); // 2047 = 11111111111b
		int y = ((positions[i] >> 11) << 1) | (symbols[i] >> 1); //  2048 = 2^11
		DIRECT_A2D_ELEM(image, y, x)++;
	}
}

template <typename T>
void EERRenderer::render4K_to_2K(MultidimArray<T> &image, std::vector<unsigned int> &positions, std::vector<unsigned char> &symbols, int n_electrons)
{
	for (int i = 0; i < n_electrons; i++)
	{
		int x = (positions[i] & 4095) >> 1; // 4095 = 111111111111b
		int y = (positions[i] >> 12) >> 1; //  4096 = 2^12
		DIRECT_A2D_ELEM(image, y, x)++;
	}
}

template <typename T>
void EERRenderer::render2K_to_2K(MultidimArray<T> &image, std::vector<unsigned int> &positions, std::vector<unsigned char> &symbols, int n_electrons)
{
	for (int i = 0; i < n_electrons; i++)
	{
		int x = positions[i] & 2047; // 2047 = 11111111111b
		int y = positions[i] >> 11; //  2048 = 2^11
		DIRECT_A2D_ELEM(image, y, x)++;
	}
}

EERRenderer::EERRenderer()
{
	ready = false;
	read_data = false;
	buf = NULL;
	preread_start = -1;
	preread_end = -1;
	eer_upsampling = 1;
}

void EERRenderer::read(FileName _fn_movie, int eer_upsampling)
{
	if (ready)
		REPORT_ERROR("Logic error: you cannot recycle EERRenderer for multiple files (now)");

	if (eer_upsampling == -1 || eer_upsampling == 1 || eer_upsampling == 2 || eer_upsampling == 3)
		this->eer_upsampling = eer_upsampling;
	else
	{
		std::cerr << "EERRenderer::read: eer_upsampling = " << eer_upsampling << std::endl;
		REPORT_ERROR("EERRenderer::read: eer_upsampling must be -1, 1, 2 or 3.");
	}

	fn_movie = _fn_movie;

	// First of all, check the file size
	FILE *fh = fopen(fn_movie.c_str(), "r");
	if (fh == NULL)
		REPORT_ERROR("Failed to open " + fn_movie);

	fseek(fh, 0, SEEK_END);
	file_size = ftell(fh);
	fseek(fh, 0, SEEK_SET);

	silenceTIFFWarnings();

	// Try reading as TIFF
	TIFF *ftiff = TIFFOpen(fn_movie.c_str(), "r");

	if (ftiff == NULL)
	{
		is_legacy = true;
		readLegacy(fh);
	}
	else
	{
		is_legacy = false;

		// Check width & size
		uint16_t compression = 0;
		TIFFGetField(ftiff, TIFFTAG_IMAGEWIDTH, &width);
		TIFFGetField(ftiff, TIFFTAG_IMAGELENGTH, &height);
		TIFFGetField(ftiff, TIFFTAG_COMPRESSION, &compression);

#ifdef DEBUG_EER
		printf("EER in TIFF: %s size = %lld, width = %d, height = %d, compression = %d\n", fn_movie.c_str(), file_size, width, height, compression);
#endif

		// TIA can write an EER file whose first page is a sum and compressoin == 1.
		// This is not supported (yet). EPU never writes such movies.
		if (compression == EERRenderer::TIFF_COMPRESSION_EER8bit)
		{
			rle_bits = 8;
			subpixel_bits = 4;
		}
		else if (compression == EERRenderer::TIFF_COMPRESSION_EER7bit)
		{
			rle_bits = 7;
			subpixel_bits = 4;
		}
		else if (compression == EERRenderer::TIFF_COMPRESSION_EERDetailed)
		{
			// See https://stackoverflow.com/questions/33522589/how-to-read-custom-tiff-tags-w-o-tifffieldinfo
			// and "AUTOREGISTERED TAGS" in https://manpages.debian.org/testing/libtiff-dev/TIFFGetField.3tiff.en.html.
			uint32_t count;
			uint16_t *rle, *subpix_h, *subpix_v;

			TIFFGetField(ftiff, EERRenderer::TIFFTAG_EER_RLE_DEPTH, &count, (void*)&rle);
			rle_bits = *rle;
			TIFFGetField(ftiff, EERRenderer::TIFFTAG_EER_SUBPIXEL_H_DEPTH, &count, (void*)&subpix_h);
			// Prototype images apparently don't have TIFFTAG_EER_SUBPIXEL_V_DEPTH
			if (TIFFGetField(ftiff, EERRenderer::TIFFTAG_EER_SUBPIXEL_V_DEPTH, &count, (void*)&subpix_v) == 0)
				subpix_v = subpix_h;

			if (rle_bits != 7 || *subpix_h != *subpix_v || *subpix_h != 1)
			{
				REPORT_ERROR("Unsupported compression scheme: type = " + integerToString(compression) +
				             ", rle_bits = " + integerToString(rle_bits) + ", subpix_h = " + integerToString(*subpix_h) +
				             ", subpix_v = " + integerToString(*subpix_v));
			}
			subpixel_bits = 1 << *subpix_h;

			if (subpixel_bits == 2 && (eer_upsampling == -1 || eer_upsampling >= 3))
			{
				REPORT_ERROR("For subpixel_bits = 2, eer_upsamling must be 1 or 2.");
			}
		}
		else
			REPORT_ERROR("Unknown compression scheme for EER: " + integerToString(compression));

		if (width != height || (width != EER_4K && width != EER_2K))
			REPORT_ERROR("Currently we support only 4096x4096 or 2048x2048 pixel EER movies.");

		total_pixels = (long long)width * height;

		// Find the number of frames
		// TODO: FIXME: an EER movie might contain a summed frame
		nframes = TIFFNumberOfDirectories(ftiff);
		TIFFClose(ftiff);
#ifdef DEBUG_EER
		printf("EER in TIFF: %s rle_bits = %d, subpixel_bits = %d, nframes = %d\n", fn_movie.c_str(), rle_bits, subpixel_bits, nframes);
#endif
	}

	fclose(fh);
	ready = true;
}

void EERRenderer::readLegacy(FILE *fh)
{
	/* Load everything first */
	RCTIC(TIMING_READ_EER);
	buf = (unsigned char*)malloc(file_size);
	if (buf == NULL)
		REPORT_ERROR("Failed to allocate the buffer.");
	if (fread(buf, sizeof(char), file_size, fh) != file_size)
		REPORT_ERROR("EERRenderer::readLegacy: Failed to read the expected size from " + fn_movie);
	RCTOC(TIMING_READ_EER);

	/* Build frame index */
	RCTIC(TIMING_BUILD_INDEX);
	long long pos = file_size;
	while (pos > 0)
	{
		pos -= EER_LEN_FOOTER;
		if (strncmp(EER_FOOTER_OK, (char*)buf + pos, EER_LEN_FOOTER) == 0)
		{
			pos -= 8;
			long long frame_size = *(long long*)(buf + pos) * sizeof(long long);
			pos -= frame_size;
			frame_starts.push_back(pos);
			frame_sizes.push_back(frame_size);
#ifdef DEBUG_EER
			printf("Frame: LAST-%5d Start at: %08d Frame size: %lld\n", frame_starts.size(), pos, frame_size);
#endif
		}
		else // if (strncmp(EER_FOOTER_ERR, (char*)buf + pos, EER_LEN_FOOTER) == 0)
		{
			REPORT_ERROR("Broken frame in file " + fn_movie);
		}
	}
	std::reverse(frame_starts.begin(), frame_starts.end());
	std::reverse(frame_sizes.begin(), frame_sizes.end());
	RCTOC(TIMING_BUILD_INDEX);

	nframes = frame_starts.size();
	read_data = true;
}

void EERRenderer::lazyReadFrames()
{
	#pragma omp critical(EERRenderer_lazyReadFrames)
	{
		if (!read_data) // cannot return from within omp critical
		{	
			TIFF *ftiff = TIFFOpen(fn_movie.c_str(), "r");

			frame_starts.resize(nframes, 0);
			frame_sizes.resize(nframes, 0);
			buf = (unsigned char*)malloc(file_size); // This is big enough
			if (buf == NULL)
				REPORT_ERROR("Failed to allocate the buffer for " + fn_movie);
			long long pos = 0;

			// Read everything
			for (int frame = 0; frame < nframes; frame++)
			{
				if ((preread_start > 0 && frame < preread_start) ||
				    (preread_end > 0 && frame > preread_end))
					continue;

				TIFFSetDirectory(ftiff, frame);
				const int nstrips = TIFFNumberOfStrips(ftiff);
				frame_starts[frame] = pos;

				for (int strip = 0; strip < nstrips; strip++)
				{
					const int strip_size = TIFFRawStripSize(ftiff, strip);
					if (pos + strip_size >= file_size)
						REPORT_ERROR("EER: buffer overflow when reading raw strips.");

					TIFFReadRawStrip(ftiff, strip, buf + pos, strip_size);
					pos += strip_size;
					frame_sizes[frame] += strip_size;
				}
	#ifdef DEBUG_EER
				printf("EER in TIFF: Read frame %d from %s, nstrips = %d, current pos in buffer = %lld / %lld\n", frame, fn_movie.c_str(), nstrips, pos, file_size);
	#endif
			}

			TIFFClose(ftiff);

			read_data = true;
		}
	}
}

EERRenderer::~EERRenderer()
{
	if (buf != NULL)
		free(buf);
}

int EERRenderer::getNFrames()
{
	if (!ready)
		REPORT_ERROR("EERRenderer::getNFrames called before ready.");

	return nframes;
}

int EERRenderer::getWidth()
{
	if (!ready)
		REPORT_ERROR("EERRenderer::getNFrames called before ready.");

	if (eer_upsampling == -1)
		return width >> 1;

	return width << (eer_upsampling - 1);
}

int EERRenderer::getHeight()
{
	if (!ready)
		REPORT_ERROR("EERRenderer::getNFrames called before ready.");

	if (eer_upsampling == -1)
		return height >> 1;

	return height << (eer_upsampling - 1);
}

template <typename T>
long long EERRenderer::renderFrames(int frame_start, int frame_end, MultidimArray<T> &image)
{
	if (!ready)
		REPORT_ERROR("EERRenderer::renderNFrames called before ready.");

	lazyReadFrames();

	if (frame_start <= 0 || frame_start > getNFrames() ||
	    frame_end < frame_start || frame_end > getNFrames())
	{
		std::cerr << "EERRenderer::renderFrames(frame_start = " << frame_start << ", frame_end = " << frame_end << "),  NFrames = " << getNFrames() << std::endl;
		REPORT_ERROR("Invalid frame range was requested.");
	}

	// Make this 0-indexed
	frame_start--;
	frame_end--;

	long long total_n_electron = 0;

	std::vector<unsigned int> positions;
	std::vector<unsigned char> symbols;
	image.initZeros(getHeight(), getWidth());

	for (int iframe = frame_start; iframe <= frame_end; iframe++)
	{
		RCTIC(TIMING_UNPACK_RLE);
		if ((preread_start > 0 && iframe < preread_start) ||
	  	    (preread_end > 0 && iframe > preread_end))
		{
			std::cerr << "EERRenderer::renderFrames(frame_start = " << frame_start + 1 << ", frame_end = " << frame_end + 1<< "),  NFrames = " << getNFrames() << " preread_start = " << preread_start + 1 << " prered_end = " << preread_end + 1<< std::endl;
			REPORT_ERROR("Tried to render frames outside pre-read region");
		}		

		long long pos = frame_starts[iframe];
		unsigned int n_pix = 0, n_electron = 0;
		const int max_electrons = frame_sizes[iframe] * 2; // at 4 bits per electron (very permissive bound!)
		if (positions.size() < max_electrons)
		{
			positions.resize(max_electrons);
			symbols.resize(max_electrons);
		}

		if (rle_bits == 7 && subpixel_bits == 4)
		{
			unsigned int bit_pos = 0; // 4 K * 4 K * 11 bit << 2 ** 32
			unsigned char p, s;

			while (true)
			{
				// Fetch 32 bits and unpack up to 2 chunks of 7 + 4 bits.
				// This is faster than unpack 7 and 4 bits sequentially.
				// Since the size of buf is larger than the actual size by the TIFF header size,
				// it is always safe to read ahead.

				long long first_byte = pos + (bit_pos >> 3);
				const unsigned int bit_offset_in_first_byte = bit_pos & 7; // 7 = 00000111 (same as % 8)
				const unsigned int chunk = (*(unsigned int*)(buf + first_byte)) >> bit_offset_in_first_byte;

				p = (unsigned char)(chunk & 127); // 127 = 01111111; 7 bits for RLE
				bit_pos += 7; // TODO: we can remove this for further speed.
				n_pix += p;
				if (n_pix >= total_pixels) break;
				if (p == 127) continue; // this should be rare.

				// 15 = 00001111; 4 bits for symbol. See the 8+4 bit section for 0x0A
				s = (unsigned char)((chunk >> 7) & 15) ^ 0x0A;
				bit_pos += 4;
				positions[n_electron] = n_pix;
				symbols[n_electron] = s;
				n_electron++;
				n_pix++;

				p = (unsigned char)((chunk >> 11) & 127);
				bit_pos += 7;
				n_pix += p;
				if (n_pix >= total_pixels) break;
				if (p == 127) continue;

				s = (unsigned char)((chunk >> 18) & 15) ^ 0x0A;
				bit_pos += 4;
				positions[n_electron] = n_pix;
				symbols[n_electron] = s;
				n_electron++;
				n_pix++;
			}
		}
		else if (rle_bits == 7 && subpixel_bits == 2)
		{
			unsigned int bit_pos = 0; // 4 K * 4 K * 11 bit << 2 ** 32
			unsigned char p, s;

			while (true)
			{
				// Fetch 32 bits and unpack up to 3 chunks of 7 + 2 bits.
				// This is faster than unpack 7 and 2 bits sequentially.
				// Since the size of buf is larger than the actual size by the TIFF header size,
				// it is always safe to read ahead.

				long long first_byte = pos + (bit_pos >> 3);
				const unsigned int bit_offset_in_first_byte = bit_pos & 7; // 7 = 00000111 (same as % 8)
				const unsigned int chunk = (*(unsigned int*)(buf + first_byte)) >> bit_offset_in_first_byte;

				p = (unsigned char)(chunk & 127); // 127 = 01111111; 7 bits for RLE
				bit_pos += 7; // TODO: we can remove this for further speed.
				n_pix += p;
				if (n_pix >= total_pixels) break;
				if (p == 127) continue; // this should be rare.

				// 3 = 00000011; 2 bits for symbol
				// Note that we have to flip bits (see below).
				s = (unsigned char)((chunk >> 7) & 3) ^ 3;
				bit_pos += 2;
				positions[n_electron] = n_pix;
				symbols[n_electron] = s;
				n_electron++;
				n_pix++;

				p = (unsigned char)((chunk >> 9) & 127);
				bit_pos += 7;
				n_pix += p;
				if (n_pix >= total_pixels) break;
				if (p == 127) continue;

				s = (unsigned char)((chunk >> 16) & 3) ^ 3;
				bit_pos += 2;
				positions[n_electron] = n_pix;
				symbols[n_electron] = s;
				n_electron++;
				n_pix++;

				p = (unsigned char)((chunk >> 18) & 127);
				bit_pos += 7;
				n_pix += p;
				if (n_pix >= total_pixels) break;
				if (p == 127) continue;

				s = (unsigned char)((chunk >> 25) & 3) ^ 3;
				bit_pos += 2;
				positions[n_electron] = n_pix;
				symbols[n_electron] = s;
				n_electron++;
				n_pix++;
			}
		}
		else if (rle_bits == 8 && subpixel_bits == 4)
		{
			// unpack every two symbols = 12 bit * 2 = 24 bit = 3 byte
			// high <- |bbbbBBBB|BBBBaaaa|AAAAAAAA| -> low
			// With SIMD intrinsics at the SSSE3 level, we can unpack 10 symbols (120 bits) simultaneously.
			unsigned char p1, p2, s1, s2;

			const long long pos_limit = frame_starts[iframe] + frame_sizes[iframe];
			// Because there is a footer, it is safe to go beyond the limit by two bytes.
			while (pos < pos_limit)
			{
				// Symbol is bit tricky: 0000YyXx, where Y and X must be flipped.
				// In other words, the bits for shifts 0, 1, 2, 3 are 10, 11, 00, 01.
				// This can be considered as 'signed 2 bit' representation of -2, -1, 0, 1.
				// For 2 bit symbols (2K EER): 000000YX and Y and X must be flipped.
				// That is, shifts 0 and 1 correspond to bits 1 and 0.
				// This is "signed 1 bit" representation of -1 and 0..
				// ref: Lingbo Yu, TFS (Email to Takanori on 10-11 May 2023)
				p1 = buf[pos];
				s1 = (buf[pos + 1] & 0x0F) ^ 0x0A; // 0x0F = 00001111, 0x0A = 00001010

				p2 = (buf[pos + 1] >> 4) | (buf[pos + 2] << 4);
				s2 = (buf[pos + 2] >> 4) ^ 0x0A;

				// Note the order. Add p before checking the size and placing a new electron.
				n_pix += p1;
				if (n_pix >= total_pixels) break;
				if (p1 < 255)
				{
					positions[n_electron] = n_pix;
					symbols[n_electron] = s1;
					n_electron++;
					n_pix++;
				}

				n_pix += p2;
				if (n_pix >= total_pixels) break;
				if (p2 < 255)
				{
					positions[n_electron] = n_pix;
					symbols[n_electron] = s2;
					n_electron++;
					n_pix++;
				}
#ifdef DEBUG_EER_DETAIL
				printf("%d: %u %u, %u %u %d\n", pos, p1, s1, p2, s2, n_pix);
#endif
				pos += 3;
			}
		}

		if (n_pix != total_pixels)
		{
			std::cerr << "WARNING: The number of pixels is not right in " + fn_movie + " frame " + integerToString(iframe + 1) + ". Probably this frame is corrupted. This frame is skipped." << std::endl;
			continue;
		}

		RCTOC(TIMING_UNPACK_RLE);

		RCTIC(TIMING_RENDER_ELECTRONS);
		if (width == EER_4K)
		{
			if (eer_upsampling == 3)
				render4K_to_16K(image, positions, symbols, n_electron);
			else if (eer_upsampling == 2)
				render4K_to_8K(image, positions, symbols, n_electron);
			else if (eer_upsampling == 1)
				render4K_to_4K(image, positions, symbols, n_electron);
			else if (eer_upsampling == -1)
				render4K_to_2K(image, positions, symbols, n_electron);
			else
				REPORT_ERROR("Invalid EER upsamle for 4K images. This must be 3, 2, 1 or -1.");
		}
		else if (width == EER_2K)
		{
			if (eer_upsampling == 2)
				render2K_to_4K(image, positions, symbols, n_electron);
			else if (eer_upsampling == 1)
				render2K_to_2K(image, positions, symbols, n_electron);
			else
				REPORT_ERROR("Invalid EER upsamle for 2K images. This must be 2 or 1.");
		}
		else
			REPORT_ERROR("Logic error: an invalid EER size at EERRenderer::renderFrames().");
		RCTOC(TIMING_RENDER_ELECTRONS);

		total_n_electron += n_electron;
#ifdef DEBUG_EER
		printf("Decoded %lld electrons / %d pixels from frame %5d.\n", n_electron, n_pix, iframe);
#endif
	}
#ifdef DEBUG_EER
	printf("Decoded %lld electrons in total.\n", total_n_electron);
#endif

#ifdef TIMING
	EERtimer.printTimes(false);
#endif

	return total_n_electron;
}

// Instantiate for Polishing
template long long EERRenderer::renderFrames<float>(int frame_start, int frame_end, MultidimArray<float> &image);
template long long EERRenderer::renderFrames<short>(int frame_start, int frame_end, MultidimArray<short> &image);
template long long EERRenderer::renderFrames<unsigned short>(int frame_start, int frame_end, MultidimArray<unsigned short> &image);
template long long EERRenderer::renderFrames<char>(int frame_start, int frame_end, MultidimArray<char> &image);
template long long EERRenderer::renderFrames<signed char>(int frame_start, int frame_end, MultidimArray<signed char> &image);
template long long EERRenderer::renderFrames<unsigned char>(int frame_start, int frame_end, MultidimArray<unsigned char> &image);
