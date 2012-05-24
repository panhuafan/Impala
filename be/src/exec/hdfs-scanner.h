// Copyright (c) 2012 Cloudera, Inc. All rights reserved.

#ifndef IMPALA_EXEC_HDFS_SCANNER_H_
#define IMPALA_EXEC_HDFS_SCANNER_H_

#include <vector>
#include <memory>
#include <stdint.h>
#include <boost/regex.hpp>
#include <boost/scoped_ptr.hpp>

#include "exec/scan-node.h"

namespace impala {

class DescriptorTbl;
class TPlanNode;
class RowBatch;
class Status;
class TupleDescriptor;
class MemPool;
class Tuple;
class SlotDescriptor;
class Expr;
class TextConverter;
class TScanRange;
class ByteStream;
class HdfsScanNode;
class Decompressor;
struct HdfsScanRange;

// Intermediate structure used for two pass parsing approach. In the first pass,
// the FieldLocation structs are filled out and contain where all the fields start and
// their lengths.  In the second pass, the FieldLocation is used to write out the
// slots. We want to keep this struct as small as possible.
struct FieldLocation {
  //start of field
  char* start;
  // Encodes the length and whether or not this fields needs to be unescaped.
  // If len < 0, then the field needs to be unescaped.
  int len;

  static const char* LLVM_CLASS_NAME;
};

// HdfsScanners are instantiated by an HdfsScanNode to parse file data in a particular
// format into Impala's Tuple structures. They are an abstract class; the actual mechanism
// for parsing bytes into tuples is format specific and supplied by subclasses.  This
// class provides methods to allocate tuple buffer data to write tuples to, and access to
// the parent scan node to retrieve information that is constant across scan range
// boundaries. The scan node provides a ByteStream* for reading bytes from (usually) an
// HdfsByteStream. Management of any buffer space to stage those bytes is the
// responsibility of the subclass.  The scan node also provides a template tuple
// containing pre-materialised slots to initialise each tuple.
// Subclasses must implement GetNext. They must also call scan_node_->IncrNumRowsReturned()
// for every row they write.
// The typical lifecycle is:
//   1. Prepare
//   2. InitCurrentScanRange
//   3. GetNext
//   4. Repeat 3 until scan range is exhausted
//   5. Repeat from 2., if there are more scan ranges in the current file.
class HdfsScanner {
 public:
  // Assumed size of a OS file block.  Used mostly when reading file format headers, etc.
  // This probably ought to be a derived number from the environment.
  const static int FILE_BLOCK_SIZE = 4096;

  // scan_node - parent scan node
  // template_tuple - the default (i.e. partition key) tuple before
  // filling materialized slots
  // tuple_pool - mem pool owned by parent scan node for allocation
  // of tuple buffer data
  HdfsScanner(HdfsScanNode* scan_node, RuntimeState* state, 
              Tuple* template_tuple, MemPool* tuple_pool);

  virtual ~HdfsScanner();

  // Writes parsed tuples into tuple buffer,
  // and sets pointers in row_batch to point to them.
  // row_batch may be non-full when all scan ranges have been read
  virtual Status GetNext(RowBatch* row_batch, bool* eos) = 0;

  // One-time initialisation of state that is constant across scan ranges.
  virtual Status Prepare();

  // One-time initialisation of per-scan-range state.  The scanner objects are
  // reused for different scan ranges so this function must reset all state.
  virtual Status InitCurrentScanRange(HdfsScanRange* scan_range,
                                      Tuple* template_tuple,
                                      ByteStream* byte_stream);

 protected:
  // For EvalConjunctsForScanner
  HdfsScanNode* scan_node_;

  // RuntimeState for error reporting
  RuntimeState* state_;

  // Contiguous block of memory into which tuples are written, allocated
  // from tuple_pool_. We don't allocate individual tuples from tuple_pool_ directly,
  // because MemPool::Allocate() always rounds up to the next 8 bytes
  // (ie, would be wasteful if we had 2-byte tuples).
  uint8_t* tuple_buffer_;

  // Size of tuple_buffer_.
  int tuple_buffer_size_;

  // Fixed size of each tuple, in bytes
  int tuple_byte_size_;

  // Tuple index in tuple row.
  int tuple_idx_;

  // Contains all memory for tuple data, including string data which
  // we can't reference in the file buffers (because it needs to be
  // unescaped or straddles two file buffers). Owned by the parent
  // scan node, since the data in it may need to outlive this scanner.
  MemPool* tuple_pool_;

  // Current tuple.
  Tuple* tuple_;

  // number of errors in current file
  int num_errors_in_file_;

  // The current provider of bytes to parse. Usually only changes
  // between files, so is active for the lifetime of a scanner.
  ByteStream* current_byte_stream_;

  // A partially materialized tuple with only partition key slots set.
  // The non-partition key slots are set to NULL.  The template tuple
  // must be copied into tuple_ before any of the other slots are
  // materialized.
  // Pointer is NULL if there are no partition key slots.
  // This template tuple is computed once for each file and valid for
  // the duration of that file.
  Tuple* template_tuple_;

  // True if the descriptor of the tuple the scanner is writing has
  // string slots and we are not compacting data. This is used to decide
  // how to treat buffer memory that contains slot data.
  bool has_noncompact_strings_;

  // The scan range currently being read
  HdfsScanRange* current_scan_range_;

  // Allocates a buffer from tuple_pool_ large enough to hold one
  // tuple for every remaining row in row_batch.  The tuple memory
  // is uninitialized.
  void AllocateTupleBuffer(RowBatch* row_batch);

  // Utility method to write out tuples when there are no materialized
  // fields (e.g. select count(*) or only partition keys).
  //   num_tuples - Total number of tuples to write out.
  // Returns the number of tuples added to the row batch.
  int WriteEmptyTuples(RowBatch* row_batch, int num_tuples);

  // Report parse error for column @ desc
  void ReportColumnParseError(const SlotDescriptor* desc, const char* data, int len);
};

}

#endif
