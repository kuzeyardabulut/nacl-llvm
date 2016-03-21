//===- NaClBitstreamReader.h -----------------------------------*- C++ -*-===//
//     Low-level bitstream reader interface
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This header defines the BitstreamReader class.  This class can be used to
// read an arbitrary bitstream, regardless of its contents.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BITCODE_NACL_NACLBITSTREAMREADER_H
#define LLVM_BITCODE_NACL_NACLBITSTREAMREADER_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/Bitcode/NaCl/NaClBitcodeHeader.h"
#include "llvm/Bitcode/NaCl/NaClLLVMBitCodes.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/StreamingMemoryObject.h"
#include <climits>
#include <vector>

namespace llvm {

class Deserializer;
class NaClBitstreamCursor;

namespace naclbitc {

/// Returns the Bit as a Byte:BitInByte string.
std::string getBitAddress(uint64_t Bit);

/// Severity levels for reporting errors.
enum ErrorLevel {
  Warning,
  Error,
  Fatal
};

// Basic printing routine to generate the beginning of an error
// message. BitPosition is the bit position the error was found.
// Level is the severity of the error.
raw_ostream &ErrorAt(raw_ostream &Out, ErrorLevel Level,
                     uint64_t BitPosition);

} // End namespace naclbitc.

/// This class is used to read from a NaCl bitcode wire format stream,
/// maintaining information that is global to decoding the entire file.
/// While a file is being read, multiple cursors can be independently
/// advanced or skipped around within the file.  These are represented by
/// the NaClBitstreamCursor class.
class NaClBitstreamReader {
public:
  // Models a raw list of abbreviations.
  static const size_t DefaultAbbrevListSize = 12;
  using AbbrevListVector = SmallVector<NaClBitCodeAbbrev *,
                                       DefaultAbbrevListSize>;

  // Models and maintains a list of abbreviations. In particular, it maintains
  // updating reference counts of abbreviation operators within the abbreviation
  // list.
  class AbbrevList {
  public:
    AbbrevList() = default;
    explicit AbbrevList(const AbbrevList &NewAbbrevs) {
      appendList(NewAbbrevs);
    }
    AbbrevList &operator=(const AbbrevList &Rhs) {
      clear();
      appendList(Rhs);
      return *this;
    }
    // Creates a new (empty) abbreviation, appends it to this, and then returns
    // the new abbreviation.
    NaClBitCodeAbbrev *appendCreate() {
      NaClBitCodeAbbrev *Abbv = new NaClBitCodeAbbrev();
      Abbrevs.push_back(Abbv);
      return Abbv;
    }
    // Appends the given abbreviation to this.
    void append(NaClBitCodeAbbrev *Abbrv) {
      Abbrv->addRef();
      Abbrevs.push_back(Abbrv);
    }
    // Appends the contents of NewAbbrevs to this.
    void appendList(const AbbrevList &NewAbbrevs) {
      for (NaClBitCodeAbbrev *Abbrv : NewAbbrevs.Abbrevs)
        append(Abbrv);
    }
    // Returns last abbreviation on list.
    NaClBitCodeAbbrev *last() { return Abbrevs.back(); }
    // Removes the last element of the list.
    void popLast() {
      Abbrevs.back()->dropRef();
      Abbrevs.pop_back();
    }
    // Empties abbreviation list.
    void clear() {
      while(!Abbrevs.empty())
        popLast();
    }
    // Allow read access to vector defining list.
    const AbbrevListVector &getVector() const { return Abbrevs; }
    ~AbbrevList() { clear(); }
  private:
    AbbrevListVector Abbrevs;
  };

  /// This contains information about abbreviations in blocks defined in the
  /// BLOCKINFO_BLOCK block.  These describe global abbreviations that apply to
  /// all succeeding blocks of the specified ID.
  class BlockInfo {
    BlockInfo &operator=(const BlockInfo&) = delete;
  public:
    BlockInfo() = default;
    explicit BlockInfo(unsigned BlockID)
        : BlockID(BlockID), Abbrevs() {}
    BlockInfo(const BlockInfo&) = default;
    unsigned getBlockID() const { return BlockID; }
    void setBlockID(unsigned ID) { BlockID = ID; }
    AbbrevList &getAbbrevs() { return Abbrevs; }
    ~BlockInfo() {}
  private:
    unsigned BlockID;
    AbbrevList Abbrevs;
  };

private:
  friend class NaClBitstreamCursor;

  std::unique_ptr<MemoryObject> BitcodeBytes;

  std::vector<BlockInfo> BlockInfoRecords;

  // True if the BlockInfo block has been read.
  bool HasReadBlockInfoBlock = false;

  /// \brief Holds the offset of the first byte after the header.
  size_t InitialAddress;

  // True if filler should be added to byte align records.
  bool AlignBitcodeRecords = false;
  NaClBitstreamReader(const NaClBitstreamReader&) = delete;
  void operator=(const NaClBitstreamReader&) = delete;


  void initFromHeader(NaClBitcodeHeader &Header) {
    InitialAddress = Header.getHeaderSize();
    AlignBitcodeRecords = Header.getAlignBitcodeRecords();
  }

public:
  /// Read stream from sequence of bytes [Start .. End) after parsing
  /// the given bitcode header.
  NaClBitstreamReader(const unsigned char *Start, const unsigned char *End,
                      NaClBitcodeHeader &Header)
      : BitcodeBytes(getNonStreamedMemoryObject(Start, End)) {
    initFromHeader(Header);
  }

  /// Read stream from Bytes, after parsing the given bitcode header.
  NaClBitstreamReader(MemoryObject *Bytes, NaClBitcodeHeader &Header)
      : BitcodeBytes(Bytes) {
    initFromHeader(Header);
  }

  /// Read stream from bytes, starting at the given initial address.
  /// Provides simple API for unit testing.
  NaClBitstreamReader(MemoryObject *Bytes, size_t InitialAddress)
      : BitcodeBytes(Bytes), InitialAddress(InitialAddress) {
  }

  // Returns the memory object that is being read.
  MemoryObject &getBitcodeBytes() { return *BitcodeBytes; }

  ~NaClBitstreamReader() {}

  /// \brief Returns the initial address (after the header) of the input stream.
  size_t getInitialAddress() const {
    return InitialAddress;
  }

  //===--------------------------------------------------------------------===//
  // Block Manipulation
  //===--------------------------------------------------------------------===//

  /// If there is block info for the specified ID, return it, otherwise return
  /// null.
  const BlockInfo *getBlockInfo(unsigned BlockID) const {
    // Common case, the most recent entry matches BlockID.
    if (!BlockInfoRecords.empty() &&
        BlockInfoRecords.back().getBlockID() == BlockID)
      return &BlockInfoRecords.back();

    for (unsigned i = 0, e = static_cast<unsigned>(BlockInfoRecords.size());
         i != e; ++i)
      if (BlockInfoRecords[i].getBlockID() == BlockID)
        return &BlockInfoRecords[i];
    return nullptr;
  }

  BlockInfo &getOrCreateBlockInfo(unsigned BlockID) {
    if (const BlockInfo *BI = getBlockInfo(BlockID))
      return *const_cast<BlockInfo*>(BI);

    // Otherwise, add a new record.
    BlockInfoRecords.push_back(BlockInfo(BlockID));
    return BlockInfoRecords.back();
  }
};

/// When advancing through a bitstream cursor, each advance can discover a few
/// different kinds of entries:
struct NaClBitstreamEntry {
  enum {
    Error,    // Malformed bitcode was found.
    EndBlock, // We've reached the end of the current block, (or the end of the
              // file, which is treated like a series of EndBlock records.
    SubBlock, // This is the start of a new subblock of a specific ID.
    Record    // This is a record with a specific AbbrevID.
  } Kind;

  unsigned ID;

  static NaClBitstreamEntry getError() {
    NaClBitstreamEntry E; E.Kind = Error; return E;
  }
  static NaClBitstreamEntry getEndBlock() {
    NaClBitstreamEntry E; E.Kind = EndBlock; return E;
  }
  static NaClBitstreamEntry getSubBlock(unsigned ID) {
    NaClBitstreamEntry E; E.Kind = SubBlock; E.ID = ID; return E;
  }
  static NaClBitstreamEntry getRecord(unsigned AbbrevID) {
    NaClBitstreamEntry E; E.Kind = Record; E.ID = AbbrevID; return E;
  }
};

/// Models default view of a bitcode record.
typedef SmallVector<uint64_t, 8> NaClBitcodeRecordVector;

/// Class NaClAbbrevListener is used to allow instances of class
/// NaClBitcodeParser to listen to record details when processing
/// abbreviations. The major reason for using a listener is that the
/// NaCl bitcode reader would require a major rewrite (including the
/// introduction of more overhead) if we were to lift abbreviations up
/// to the bitcode reader. That is, not only would we have to lift the
/// block processing up into the readers (i.e. many blocks in
/// NaClBitcodeReader and NaClBitcodeParser), but add many new API's
/// to allow the readers to update internals of the bit stream reader
/// appropriately.
class NaClAbbrevListener {
  NaClAbbrevListener(const NaClAbbrevListener&) = delete;
  void operator=(const NaClAbbrevListener&) = delete;
public:
  NaClAbbrevListener() {}
  virtual ~NaClAbbrevListener() {}

  /// Called to process the read abbreviation.
  virtual void ProcessAbbreviation(NaClBitCodeAbbrev *Abbrv,
                                   bool IsLocal) = 0;

  /// Called after entering block. NumWords is the number of words
  /// in the block.
  virtual void BeginBlockInfoBlock(unsigned NumWords) = 0;

  /// Called if a naclbitc::BLOCKINFO_CODE_SETBID record is found in
  /// NaClBitstreamCursor::ReadBlockInfoBlock.
  virtual void SetBID() = 0;

  /// Called just before an EndBlock record is processed by
  /// NaClBitstreamCursor::ReadBlockInfoBlock
  virtual void EndBlockInfoBlock() = 0;

  /// The values of the bitcode record associated with the called
  /// virtual function.
  NaClBitcodeRecordVector Values;

  /// Start bit for current record being processed in
  /// NaClBitstreamCursor::ReadBlockInfoBlock.
  uint64_t StartBit;
};

/// This represents a position within a bitcode file. There may be multiple
/// independent cursors reading within one bitstream, each maintaining their
/// own local state.
///
/// Unlike iterators, NaClBitstreamCursors are heavy-weight objects
/// that should not be passed by value.
class NaClBitstreamCursor {
public:
  /// This class handles errors in the bitstream reader. Redirects
  /// fatal error messages to virtual method Fatal.
  class ErrorHandler {
    ErrorHandler(const ErrorHandler &) = delete;
    ErrorHandler &operator=(const ErrorHandler &) = delete;
  public:
    explicit ErrorHandler(NaClBitstreamCursor &Cursor) : Cursor(Cursor) {}
    LLVM_ATTRIBUTE_NORETURN
    virtual void Fatal(const std::string &ErrorMessage) const;
    virtual ~ErrorHandler() {}
    uint64_t getCurrentBitNo() const {
      return Cursor.GetCurrentBitNo();
    }
  private:
    NaClBitstreamCursor &Cursor;
  };

private:
  friend class Deserializer;
  NaClBitstreamReader *BitStream;
  size_t NextChar;
  // The current error handler for the bitstream reader.
  std::unique_ptr<ErrorHandler> ErrHandler;

  // The size of the bitcode. 0 if we don't know it yet.
  size_t Size;

  /// This is the current data we have pulled from the stream but have not
  /// returned to the client. This is specifically and intentionally defined to
  /// follow the word size of the host machine for efficiency. We use word_t in
  /// places that are aware of this to make it perfectly explicit what is going
  /// on.
  typedef size_t word_t;
  word_t CurWord;

  /// This is the number of bits in CurWord that are valid. This
  /// is always from [0...bits_of(word_t)-1] inclusive.
  unsigned BitsInCurWord;

  // Data specific to a block being scanned.
  class Block {
  public:
    Block() = delete;
    Block &operator=(const Block &Rhs) {
      GlobalAbbrevs = Rhs.GlobalAbbrevs;
      NumGlobalAbbrevs = Rhs.NumGlobalAbbrevs;
      LocalAbbrevs = Rhs.LocalAbbrevs;
      CodeAbbrev = Rhs.CodeAbbrev;
      return *this;
    }
    Block(NaClBitstreamReader::BlockInfo *GlobalAbbrevs,
          NaClBitcodeSelectorAbbrev& CodeAbbrev)
        : GlobalAbbrevs(GlobalAbbrevs),
          NumGlobalAbbrevs(GlobalAbbrevs->getAbbrevs().getVector().size()),
          LocalAbbrevs(), CodeAbbrev(CodeAbbrev) {}
    Block(NaClBitstreamReader::BlockInfo *GlobalAbbrevs)
        : GlobalAbbrevs(GlobalAbbrevs),
          NumGlobalAbbrevs(GlobalAbbrevs->getAbbrevs().getVector().size()),
          LocalAbbrevs(), CodeAbbrev() {}
    ~Block() = default;
    const NaClBitstreamReader::AbbrevList &getGlobalAbbrevs() const {
      return GlobalAbbrevs->getAbbrevs();
    }
    unsigned getNumGlobalAbbrevs() const { return NumGlobalAbbrevs; }
    const NaClBitstreamReader::AbbrevList &getLocalAbbrevs() const {
      return LocalAbbrevs;
    }
    const NaClBitcodeSelectorAbbrev &getCodeAbbrev() const {
      return CodeAbbrev;
    }
    void setCodeAbbrev(NaClBitcodeSelectorAbbrev &Abbrev) {
      CodeAbbrev = Abbrev;
    }
    NaClBitCodeAbbrev *appendLocalCreate() {
      return LocalAbbrevs.appendCreate();
    }
    void moveLocalAbbrevToAbbrevList(NaClBitstreamReader::AbbrevList *List) {
      if (List != &LocalAbbrevs) {
        NaClBitCodeAbbrev *Abbv = LocalAbbrevs.last();
        List->append(Abbv);
        LocalAbbrevs.popLast();
      }
    }
  private:
    friend class NaClBitstreamCursor;
    // The global abbreviations associated with this scope.
    NaClBitstreamReader::BlockInfo *GlobalAbbrevs;
    // Number of abbreviations when block was entered. Used to limit scope of
    // CurBlockInfo, since any abbreviation added inside a BlockInfo block
    // (within this block) must not effect global abbreviations.
    unsigned NumGlobalAbbrevs;
    NaClBitstreamReader::AbbrevList LocalAbbrevs;
    // This is the declared size of code values used for the current block, in
    // bits.
    NaClBitcodeSelectorAbbrev CodeAbbrev;
  };

  /// This tracks the Block-specific information for each nested block.
  SmallVector<Block, 8> BlockScope;

  NaClBitstreamCursor(const NaClBitstreamCursor &) = delete;
  NaClBitstreamCursor &operator=(const NaClBitstreamCursor &) = delete;

public:
  NaClBitstreamCursor() : ErrHandler(new ErrorHandler(*this)) {
    init(nullptr);
  }

  explicit NaClBitstreamCursor(NaClBitstreamReader &R)
      : ErrHandler(new ErrorHandler(*this)) { init(&R); }

  void init(NaClBitstreamReader *R) {
    freeState();
    BitStream = R;
    NextChar = (BitStream == nullptr) ? 0 : BitStream->getInitialAddress();
    Size = 0;
    BitsInCurWord = 0;
    if (BitStream) {
      BlockScope.push_back(
          Block(&BitStream->getOrCreateBlockInfo(naclbitc::TOP_LEVEL_BLOCKID)));
    }
  }

  ~NaClBitstreamCursor() {
    freeState();
  }

  void freeState() {
    while (!BlockScope.empty())
      BlockScope.pop_back();
  }

  // Replaces the current bitstream error handler with the new
  // handler. Takes ownership of the new handler and deletes it when
  // it is no longer needed.
  void setErrorHandler(std::unique_ptr<ErrorHandler> &NewHandler) {
    ErrHandler = std::move(NewHandler);
  }

  bool canSkipToPos(size_t pos) const {
    // pos can be skipped to if it is a valid address or one byte past the end.
    return pos == 0 || BitStream->getBitcodeBytes().isValidAddress(
        static_cast<uint64_t>(pos - 1));
  }

  bool AtEndOfStream() {
    if (BitsInCurWord != 0)
      return false;
    if (Size != 0)
      return Size == NextChar;
    fillCurWord();
    return BitsInCurWord == 0;
  }

  /// Return the number of bits used to encode an abbrev #.
  unsigned getAbbrevIDWidth() const {
    return BlockScope.back().getCodeAbbrev().NumBits;
  }

  /// Return the bit # of the bit we are reading.
  uint64_t GetCurrentBitNo() const {
    return NextChar*CHAR_BIT - BitsInCurWord;
  }

  NaClBitstreamReader *getBitStreamReader() {
    return BitStream;
  }
  const NaClBitstreamReader *getBitStreamReader() const {
    return BitStream;
  }

  /// Returns the current bit address (string) of the bit cursor.
  std::string getCurrentBitAddress() const {
    return naclbitc::getBitAddress(GetCurrentBitNo());
  }

  /// Flags that modify the behavior of advance().
  enum {
    /// If this flag is used, the advance() method does not automatically pop
    /// the block scope when the end of a block is reached.
    AF_DontPopBlockAtEnd = 1,

    /// If this flag is used, abbrev entries are returned just like normal
    /// records.
    AF_DontAutoprocessAbbrevs = 2
  };

  /// Advance the current bitstream, returning the next entry in the stream.
  /// Use the given abbreviation listener (if provided).
  NaClBitstreamEntry advance(unsigned Flags, NaClAbbrevListener *Listener) {
    while (1) {
      unsigned Code = ReadCode();
      if (Code == naclbitc::END_BLOCK) {
        // Pop the end of the block unless Flags tells us not to.
        if (!(Flags & AF_DontPopBlockAtEnd) && ReadBlockEnd())
          return NaClBitstreamEntry::getError();
        return NaClBitstreamEntry::getEndBlock();
      }

      if (Code == naclbitc::ENTER_SUBBLOCK)
        return NaClBitstreamEntry::getSubBlock(ReadSubBlockID());

      if (Code == naclbitc::DEFINE_ABBREV &&
          !(Flags & AF_DontAutoprocessAbbrevs)) {
        // We read and accumulate abbrev's, the client can't do anything with
        // them anyway.
        ReadAbbrevRecord(true, Listener);
        continue;
      }

      return NaClBitstreamEntry::getRecord(Code);
    }
  }

  /// This is a convenience function for clients that don't expect any
  /// subblocks. This just skips over them automatically.
  NaClBitstreamEntry advanceSkippingSubblocks(unsigned Flags = 0) {
    while (1) {
      // If we found a normal entry, return it.
      NaClBitstreamEntry Entry = advance(Flags, 0);
      if (Entry.Kind != NaClBitstreamEntry::SubBlock)
        return Entry;

      // If we found a sub-block, just skip over it and check the next entry.
      if (SkipBlock())
        return NaClBitstreamEntry::getError();
    }
  }

  /// Reset the stream to the specified bit number.
  void JumpToBit(uint64_t BitNo) {
    uintptr_t ByteNo = uintptr_t(BitNo/8) & ~(sizeof(word_t)-1);
    unsigned WordBitNo = unsigned(BitNo & (sizeof(word_t)*8-1));
    if (!canSkipToPos(ByteNo))
      reportInvalidJumpToBit(BitNo);

    // Move the cursor to the right word.
    NextChar = ByteNo;
    BitsInCurWord = 0;

    // Skip over any bits that are already consumed.
    if (WordBitNo)
      Read(WordBitNo);
  }

  void fillCurWord() {
    assert(Size == 0 || NextChar < (unsigned)Size);

    // Read the next word from the stream.
    uint8_t Array[sizeof(word_t)] = {0};

    uint64_t BytesRead =
        BitStream->getBitcodeBytes().readBytes(Array, sizeof(Array), NextChar);

    // If we run out of data, stop at the end of the stream.
    if (BytesRead == 0) {
      Size = NextChar;
      return;
    }

    CurWord =
        support::endian::read<word_t, support::little, support::unaligned>(
            Array);
    NextChar += BytesRead;
    BitsInCurWord = BytesRead * 8;
  }

  word_t Read(unsigned NumBits) {
    static const unsigned BitsInWord = sizeof(word_t) * 8;

    assert(NumBits && NumBits <= BitsInWord &&
           "Cannot return zero or more than BitsInWord bits!");

    static const unsigned Mask = sizeof(word_t) > 4 ? 0x3f : 0x1f;

    // If the field is fully contained by CurWord, return it quickly.
    if (BitsInCurWord >= NumBits) {
      word_t R = CurWord & (~word_t(0) >> (BitsInWord - NumBits));

      // Use a mask to avoid undefined behavior.
      CurWord >>= (NumBits & Mask);

      BitsInCurWord -= NumBits;
      return R;
    }

    word_t R = BitsInCurWord ? CurWord : 0;
    unsigned BitsLeft = NumBits - BitsInCurWord;

    fillCurWord();

    // If we run out of data, stop at the end of the stream.
    if (BitsLeft > BitsInCurWord)
      return 0;

    word_t R2 = CurWord & (~word_t(0) >> (BitsInWord - BitsLeft));

    // Use a mask to avoid undefined behavior.
    CurWord >>= (BitsLeft & Mask);

    BitsInCurWord -= BitsLeft;

    R |= R2 << (NumBits - BitsLeft);

    return R;
  }

  uint32_t ReadVBR(unsigned NumBits) {
    uint32_t Piece = Read(NumBits);
    if ((Piece & (1U << (NumBits-1))) == 0)
      return Piece;

    uint32_t Result = 0;
    unsigned NextBit = 0;
    while (1) {
      Result |= (Piece & ((1U << (NumBits-1))-1)) << NextBit;

      if ((Piece & (1U << (NumBits-1))) == 0)
        return Result;

      NextBit += NumBits-1;
      Piece = Read(NumBits);
    }
  }

  // Read a VBR that may have a value up to 64-bits in size. The chunk size of
  // the VBR must still be <= 32 bits though.
  uint64_t ReadVBR64(unsigned NumBits) {
    uint32_t Piece = Read(NumBits);
    if ((Piece & (1U << (NumBits-1))) == 0)
      return uint64_t(Piece);

    uint64_t Result = 0;
    unsigned NextBit = 0;
    while (1) {
      Result |= uint64_t(Piece & ((1U << (NumBits-1))-1)) << NextBit;

      if ((Piece & (1U << (NumBits-1))) == 0)
        return Result;

      NextBit += NumBits-1;
      Piece = Read(NumBits);
    }
  }

private:
  void SkipToByteBoundary() {
    unsigned BitsToSkip = BitsInCurWord % CHAR_BIT;
    if (BitsToSkip) {
      CurWord >>= BitsToSkip;
      BitsInCurWord -= BitsToSkip;
    }
  }

  void SkipToByteBoundaryIfAligned() {
    if (BitStream->AlignBitcodeRecords)
      SkipToByteBoundary();
  }

  void SkipToFourByteBoundary() {
    // If word_t is 64-bits and if we've read less than 32 bits, just dump
    // the bits we have up to the next 32-bit boundary.
    if (sizeof(word_t) > 4 &&
        BitsInCurWord >= 32) {
      CurWord >>= BitsInCurWord-32;
      BitsInCurWord = 32;
      return;
    }

    BitsInCurWord = 0;
  }
public:

  unsigned ReadCode() {
    const NaClBitcodeSelectorAbbrev &CodeAbbrev =
        BlockScope.back().getCodeAbbrev();
    return CodeAbbrev.IsFixed
        ? Read(CodeAbbrev.NumBits)
        : ReadVBR(CodeAbbrev.NumBits);
  }

  // Block header:
  //    [ENTER_SUBBLOCK, blockid, newcodelen, <align4bytes>, blocklen]

  /// Having read the ENTER_SUBBLOCK code, read the BlockID for the block.
  unsigned ReadSubBlockID() {
    return ReadVBR(naclbitc::BlockIDWidth);
  }

  /// Having read the ENTER_SUBBLOCK abbrevid and a BlockID, skip over the body
  /// of this block. If the block record is malformed, return true.
  bool SkipBlock() {
    // Read and ignore the codelen value.  Since we are skipping this block, we
    // don't care what code widths are used inside of it.
    ReadVBR(naclbitc::CodeLenWidth);
    SkipToFourByteBoundary();
    unsigned NumFourBytes = Read(naclbitc::BlockSizeWidth);

    // Check that the block wasn't partially defined, and that the offset isn't
    // bogus.
    size_t SkipTo = GetCurrentBitNo() + NumFourBytes*4*8;
    if (AtEndOfStream() || !canSkipToPos(SkipTo/8))
      return true;

    JumpToBit(SkipTo);
    return false;
  }

  /// Having read the ENTER_SUBBLOCK abbrevid, enter the block, and return true
  /// if the block has an error.
  bool EnterSubBlock(unsigned BlockID, unsigned *NumWordsP = nullptr);

  bool ReadBlockEnd() {
    if (BlockScope.empty()) return true;

    // Block tail:
    //    [END_BLOCK, <align4bytes>]
    SkipToFourByteBoundary();

    BlockScope.pop_back();
    return false;
  }

private:

  //===--------------------------------------------------------------------===//
  // Record Processing
  //===--------------------------------------------------------------------===//

private:
  // Returns abbreviation encoding associated with Value.
  NaClBitCodeAbbrevOp::Encoding getEncoding(uint64_t Value);

  void skipAbbreviatedField(const NaClBitCodeAbbrevOp &Op);

  // Reads the next Value using the abbreviation Op. Returns true only
  // if Op is an array (and sets Value to the number of elements in the
  // array).
  inline bool readRecordAbbrevField(const NaClBitCodeAbbrevOp &Op,
                                    uint64_t &Value);

  // Reads and returns the next value using the abbreviation Op,
  // assuming Op appears after an array abbreviation.
  inline uint64_t readArrayAbbreviatedField(const NaClBitCodeAbbrevOp &Op);

  // Reads the array abbreviation Op, NumArrayElements times, putting
  // the read values in Vals.
  inline void readArrayAbbrev(const NaClBitCodeAbbrevOp &Op,
                              unsigned NumArrayElements,
                              SmallVectorImpl<uint64_t> &Vals);

  // Reports that that abbreviation Index is not valid.
  void reportInvalidAbbrevNumber(unsigned Index) const;

  // Reports that jumping to Bit is not valid.
  void reportInvalidJumpToBit(uint64_t Bit) const;

public:

  /// Return the abbreviation for the specified AbbrevId.
  const NaClBitCodeAbbrev *getAbbrev(unsigned AbbrevID) const {
    unsigned AbbrevNo = AbbrevID-naclbitc::FIRST_APPLICATION_ABBREV;
    const Block &CurBlock = BlockScope.back();
    const unsigned NumGlobalAbbrevs = CurBlock.getNumGlobalAbbrevs();
    if (AbbrevNo < NumGlobalAbbrevs)
      return CurBlock.getGlobalAbbrevs().getVector()[AbbrevNo];
    unsigned LocalAbbrevNo = AbbrevNo - NumGlobalAbbrevs;
    NaClBitstreamReader::AbbrevListVector
        LocalAbbrevs = CurBlock.getLocalAbbrevs().getVector();
    if (LocalAbbrevNo >= LocalAbbrevs.size())
      reportInvalidAbbrevNumber(AbbrevID);
    return LocalAbbrevs[LocalAbbrevNo];
  }

  /// Read the current record and discard it.
  void skipRecord(unsigned AbbrevID);
  
  unsigned readRecord(unsigned AbbrevID, SmallVectorImpl<uint64_t> &Vals);

  //===--------------------------------------------------------------------===//
  // Abbrev Processing
  //===--------------------------------------------------------------------===//
  // IsLocal indicates where the abbreviation occurs. If it is in the
  // BlockInfo block, IsLocal is false. In all other cases, IsLocal is
  // true.
  void ReadAbbrevRecord(bool IsLocal,
                        NaClAbbrevListener *Listener);

  // Skips over an abbreviation record. Duplicates code of ReadAbbrevRecord,
  // except that no abbreviation is built.
  void SkipAbbrevRecord();
  
  bool ReadBlockInfoBlock(NaClAbbrevListener *Listener);
};

} // End llvm namespace

#endif
