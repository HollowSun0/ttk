#include "TopologicalCompression.h"

// General.
ttk::TopologicalCompression::TopologicalCompression() {
  this->setDebugMsgPrefix("TopologicalCompression");
}

// Dependencies.

#ifdef TTK_ENABLE_ZFP

#include <zfp.h>

int ttk::TopologicalCompression::CompressWithZFP(FILE *file,
                                                 const bool decompress,
                                                 std::vector<double> &array,
                                                 const int nx,
                                                 const int ny,
                                                 const int nz,
                                                 const double zfpTolerance) {

  int n1 = 0, n2 = 0;
  bool is2D = nx == 1 || ny == 1 || nz == 1;
  if(is2D) {
    if(nx + ny == 2 || ny + nz == 2 || nx + nz == 2) {
      this->printErr("One-dimensional arrays not supported.");
      return 0;
    }

    n1 = nx != 1 ? nx : ny;
    n2 = nx != 1 && ny != 1 ? ny : nz;
  }

  // array scalar type
  const auto type = zfp_type_double;

  // array meta data
  zfp_field *field;
  if(is2D) {
    field = zfp_field_2d(array.data(), type, n1, n2);
  } else {
    field = zfp_field_3d(array.data(), type, nx, ny, nz);
  }

  // compressed stream
  auto *zfp = zfp_stream_open(nullptr);

  // set compression mode and parameters via one of three functions (c.f.
  // https://zfp.readthedocs.io/en/release0.5.5/modes.html)
  // use fixed-accuracy mode
  zfp_stream_set_accuracy(zfp, zfpTolerance);

  // allocate buffer for compressed data
  const auto bufsize = zfp_stream_maximum_size(zfp, field);
  std::vector<unsigned char> buffer(bufsize);

  // associate bit stream with allocated buffer
  auto *stream = stream_open(buffer.data(), bufsize);
  zfp_stream_set_bit_stream(zfp, stream);
  zfp_stream_rewind(zfp);

  // return value: 0 = success
  int status = 0;

  // byte size of compressed stream
  size_t zfpsize{};

  // compress or decompress entire array
  if(decompress) {
    // read compressed stream and decompress array
    zfpsize += fread(buffer.data(), 1, bufsize, file);

    // read the ZFP header (from v2)
    const auto res = zfp_read_header(zfp, field, ZFP_HEADER_FULL);
    if(res == 0) {
      this->printErr("Could not read ZFP header");
      status = 1;
    }

    if(!zfp_decompress(zfp, field)) {
      this->printErr("Decompression failed");
      status = 1;
    }
  } else {
    // write the ZFP header
    const auto res = zfp_write_header(zfp, field, ZFP_HEADER_FULL);
    if(res == 0) {
      this->printErr("Could not write ZFP header");
      status = 1;
    }

    // compress array and output compressed stream
    zfpsize += zfp_compress(zfp, field);
    if(!zfpsize) {
      this->printErr("Compression failed");
      status = 1;
    } else
      fwrite(buffer.data(), 1, zfpsize, file);
  }

  // clean up
  zfp_field_free(field);
  zfp_stream_close(zfp);
  stream_close(stream);

  if(status != 0) {
    this->printErr("Encountered a problem with ZFP.");
  }

  return (int)zfpsize;
}

#endif // TTK_ENABLE_ZFP

#ifdef TTK_ENABLE_ZLIB

#include <zlib.h>

unsigned long
  ttk::TopologicalCompression::GetZlibDestLen(const unsigned long sourceLen) {
  return compressBound(sourceLen);
}

void ttk::TopologicalCompression::CompressWithZlib(
  bool decompress,
  unsigned char *dest,
  unsigned long &destLen,
  const unsigned char *const source,
  const unsigned long sourceLen) {

  if(decompress)
    uncompress(dest, &destLen, source, sourceLen);
  else
    compress(dest, &destLen, source, sourceLen);
}

#endif // TTK_ENABLE_ZLIB

unsigned int ttk::TopologicalCompression::log2(int val) {
  if(val == 0)
    return UINT_MAX;
  if(val == 1)
    return 0;
  unsigned int ret = 0;
  while(val > 1) {
    val >>= 1;
    ret++;
  }
  return ret;
}

// IO.

int ttk::TopologicalCompression::ReadCompactSegmentation(
  FILE *fm,
  std::vector<int> &segmentation,
  int &numberOfVertices,
  int &numberOfSegments) {

  int numberOfBytesRead = 0;

  numberOfBytesRead += sizeof(int);
  numberOfVertices = Read<int>(fm);

  numberOfBytesRead += sizeof(int);
  numberOfSegments = Read<int>(fm);

  unsigned int numberOfBitsPerSegment = log2(numberOfSegments) + 1;

#ifndef TTK_ENABLE_KAMIKAZE
  // avoid left shift with negative operand
  if(numberOfBitsPerSegment == 0) {
    return -2;
  }
#endif // TTK_ENABLE_KAMIKAZE

  // [MEDIUM] TODO: support long int
  if(numberOfBitsPerSegment > 32)
    return -3;

  // Decode
  int currentCell = 0;
  int offset = 0;
  int maskerRank = 0;
  int oldCompressedInt = 0;

  while(currentCell < numberOfVertices) {

    int compressedInt;
    numberOfBytesRead += sizeof(int);
    compressedInt = Read<int>(fm);

    while(offset + numberOfBitsPerSegment <= 32) {

      // Size of segment < size of compressed int
      int currentSegment = compressedInt;

      if(maskerRank == 0) {
        currentSegment <<= (32 - offset - numberOfBitsPerSegment);

        if(currentSegment < 0) {
          currentSegment &= 2147483647;
          currentSegment >>= (32 - numberOfBitsPerSegment);
          currentSegment |= (1 << (numberOfBitsPerSegment - 1));
        } else {
          currentSegment >>= (32 - numberOfBitsPerSegment);
        }

        offset += numberOfBitsPerSegment;
      }

      // Got overlapping mask.
      else {
        currentSegment <<= (32 - offset);
        if(currentSegment < 0) {
          currentSegment &= 2147483647;
          currentSegment >>= (32 - numberOfBitsPerSegment);
          currentSegment |= (1 << (numberOfBitsPerSegment - 1));
        } else {
          currentSegment >>= (32 - numberOfBitsPerSegment);
        }

        int nextSegment = currentSegment;

        if(oldCompressedInt < 0) {
          oldCompressedInt &= 2147483647;
          oldCompressedInt >>= maskerRank;
          oldCompressedInt |= (1 << (32 - maskerRank - 1));
        } else {
          oldCompressedInt >>= maskerRank;
        }

        currentSegment = nextSegment | oldCompressedInt;
        maskerRank = 0;
      }

      segmentation.push_back(currentSegment);
      currentCell++; // Go to next cell.
    }

    // Overlapping mask into next container.
    {
      oldCompressedInt = compressedInt;
      if(offset == 32) {
        maskerRank = 0;
      } else {
        maskerRank = offset;
        offset += numberOfBitsPerSegment;
      }
      offset %= 32;
    }
  }

  return numberOfBytesRead;
}

// Returns number of bytes written.
int ttk::TopologicalCompression::WriteCompactSegmentation(
  FILE *fm,
  const std::vector<int> &segmentation,
  int numberOfVertices,
  int numberOfSegments) {

  int numberOfBytesWritten = 0;

  // Compute number of bits per segment
  // (can be deduced at read-time from numberOfSegments)
  unsigned int numberOfBitsPerSegment = log2(numberOfSegments) + 1;

  // [MEDIUM] TODO: support long int
  if(numberOfBitsPerSegment > 32)
    return -3;

  // Encode
  int currentCell = 0;
  int offset = 0;
  int maskerRank = 0;
  while(currentCell < numberOfVertices) {

    // Create next container.
    int compressedInt = 0;

    // Write regularly all segments until one overlaps
    // two containers.
    while(offset + numberOfBitsPerSegment <= 32) {

      // out-of-bounds here if segmentation.size() == numberOfVertices
      // (segmentation allocated in compressForPersistenceDiagram)
      int currentSegment = segmentation[currentCell];

      // If applicable, fill last part of current segment.
      if(maskerRank != 0) {
        // Always a positive int.
        currentSegment = currentSegment >> (numberOfBitsPerSegment - offset);
        // (numberOfBitsPerSegment - maskerRank - 1);
        maskerRank = 0;
        compressedInt |= currentSegment;

      } else {
        int cursor = (currentSegment << offset); // 0es after <<
        compressedInt |= cursor;
        offset += numberOfBitsPerSegment;
      }

      currentCell++; // Next cell.
    }

    // Test for overflow filling last part of current container.
    if(currentCell >= numberOfVertices) {
      numberOfBytesWritten += sizeof(int);
      Write(fm, compressedInt);
      break;
    }

    // Write current segment into last part of current container,
    // to be continued into next container.
    {
      int currentSegment = segmentation[currentCell];

      if(offset == 32) {
        // currentCell++;
      } else {
        int cursor = (currentSegment << offset);
        compressedInt = compressedInt | cursor;

        maskerRank = 32 - offset;
        offset += numberOfBitsPerSegment;
      }

      offset %= 32;
    }

    // Dump current container.
    numberOfBytesWritten += sizeof(int);
    Write(fm, compressedInt);
  }

  return numberOfBytesWritten;
}

int ttk::TopologicalCompression::ReadPersistenceIndex(
  FILE *fm,
  std::vector<std::tuple<double, int>> &mappings,
  std::vector<std::tuple<double, int>> &mappingsSortedPerValue,
  std::vector<std::tuple<int, double, int>> &constraints,
  double &min,
  double &max,
  int &nbConstraints) {
  int numberOfBytesRead = 0;

  // 1.a. Read mapping.
  int mappingSize;
  numberOfBytesRead += sizeof(int);
  mappingSize = Read<int>(fm);

  for(int i = 0; i < mappingSize; ++i) {
    int idv;
    numberOfBytesRead += sizeof(int);
    idv = Read<int>(fm);

    double value;
    numberOfBytesRead += sizeof(double);
    value = Read<double>(fm);

    mappings.push_back(std::make_tuple(value, idv));
    mappingsSortedPerValue.push_back(std::make_tuple(value, idv));
  }

  // Sort mapping.
  std::sort(mappings.begin(), mappings.end(), cmp);
  std::sort(mappingsSortedPerValue.begin(), mappingsSortedPerValue.end(), cmp2);

  // 1.b. Read constraints.
  numberOfBytesRead += sizeof(int);
  nbConstraints = Read<int>(fm);

  for(int i = 0; i < nbConstraints; ++i) {
    int idVertex;
    double value;
    int vertexType;

    numberOfBytesRead += sizeof(int);
    idVertex = Read<int>(fm);

    numberOfBytesRead += sizeof(double);
    value = Read<double>(fm);

    numberOfBytesRead += sizeof(int);
    vertexType = Read<int>(fm);

    if(i == 0) {
      min = value;
      max = value;
    }

    if(value < min)
      min = value;
    if(value > max)
      max = value;

    constraints.push_back(std::make_tuple(idVertex, value, vertexType));
  }

  return numberOfBytesRead;
}

int ttk::TopologicalCompression::WritePersistenceIndex(
  FILE *fm,
  std::vector<std::tuple<double, int>> &mapping,
  std::vector<std::tuple<int, double, int>> &constraints) {
  int numberOfBytesWritten = 0;

  // Size.
  auto mappingSize = (int)mapping.size();
  numberOfBytesWritten += sizeof(int);
  Write(fm, mappingSize);

  // Segmentation values for each particular index.
  for(int i = 0; i < mappingSize; ++i) {
    std::tuple<double, int> t = mapping[i];
    int idv = std::get<1>(t);
    numberOfBytesWritten += sizeof(int);
    Write(fm, idv);

    auto value = std::get<0>(t);
    numberOfBytesWritten += sizeof(double);
    Write(fm, value);
  }

  auto nbConstraints = (int)constraints.size();
  numberOfBytesWritten += sizeof(int);
  Write(fm, nbConstraints);

  for(int i = 0; i < nbConstraints; ++i) {
    std::tuple<int, double, int> t = constraints[i];
    int idVertex = std::get<0>(t);
    auto value = std::get<1>(t);
    int vertexType = std::get<2>(t);

    numberOfBytesWritten += sizeof(int);
    Write(fm, idVertex);

    numberOfBytesWritten += sizeof(double);
    Write(fm, value);

    numberOfBytesWritten += sizeof(int);
    Write(fm, vertexType);
  }

  return numberOfBytesWritten;
}
