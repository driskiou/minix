//===- lib/MC/MCDwarf.cpp - MCDwarf implementation ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCDwarf.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Config/config.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

MCSymbol *mcdwarf::emitListsTableHeaderStart(MCStreamer &S) {
  MCSymbol *Start = S.getContext().createTempSymbol("debug_list_header_start");
  MCSymbol *End = S.getContext().createTempSymbol("debug_list_header_end");
  auto DwarfFormat = S.getContext().getDwarfFormat();
  if (DwarfFormat == dwarf::DWARF64) {
    S.AddComment("DWARF64 mark");
    S.emitInt32(dwarf::DW_LENGTH_DWARF64);
  }
  S.AddComment("Length");
  S.emitAbsoluteSymbolDiff(End, Start,
                           dwarf::getDwarfOffsetByteSize(DwarfFormat));
  S.emitLabel(Start);
  S.AddComment("Version");
  S.emitInt16(S.getContext().getDwarfVersion());
  S.AddComment("Address size");
  S.emitInt8(S.getContext().getAsmInfo()->getCodePointerSize());
  S.AddComment("Segment selector size");
  S.emitInt8(0);
  return End;
}

/// Manage the .debug_line_str section contents, if we use it.
class llvm::MCDwarfLineStr {
  MCSymbol *LineStrLabel = nullptr;
  StringTableBuilder LineStrings{StringTableBuilder::DWARF};
  bool UseRelocs = false;

public:
  /// Construct an instance that can emit .debug_line_str (for use in a normal
  /// v5 line table).
  explicit MCDwarfLineStr(MCContext &Ctx) {
    UseRelocs = Ctx.getAsmInfo()->doesDwarfUseRelocationsAcrossSections();
    if (UseRelocs)
      LineStrLabel =
          Ctx.getObjectFileInfo()->getDwarfLineStrSection()->getBeginSymbol();
  }

  /// Emit a reference to the string.
  void emitRef(MCStreamer *MCOS, StringRef Path);

  /// Emit the .debug_line_str section if appropriate.
  void emitSection(MCStreamer *MCOS);
};

static inline uint64_t ScaleAddrDelta(MCContext &Context, uint64_t AddrDelta) {
  unsigned MinInsnLength = Context.getAsmInfo()->getMinInstAlignment();
  if (MinInsnLength == 1)
    return AddrDelta;
  if (AddrDelta % MinInsnLength != 0) {
    // TODO: report this error, but really only once.
    ;
  }
  return AddrDelta / MinInsnLength;
}

//
// This is called when an instruction is assembled into the specified section
// and if there is information from the last .loc directive that has yet to have
// a line entry made for it is made.
//
void MCDwarfLineEntry::make(MCStreamer *MCOS, MCSection *Section) {
  if (!MCOS->getContext().getDwarfLocSeen())
    return;

  // Create a symbol at in the current section for use in the line entry.
  MCSymbol *LineSym = MCOS->getContext().createTempSymbol();
  // Set the value of the symbol to use for the MCDwarfLineEntry.
  MCOS->emitLabel(LineSym);

  // Get the current .loc info saved in the context.
  const MCDwarfLoc &DwarfLoc = MCOS->getContext().getCurrentDwarfLoc();

  // Create a (local) line entry with the symbol and the current .loc info.
  MCDwarfLineEntry LineEntry(LineSym, DwarfLoc);

  // clear DwarfLocSeen saying the current .loc info is now used.
  MCOS->getContext().clearDwarfLocSeen();

  // Add the line entry to this section's entries.
  MCOS->getContext()
      .getMCDwarfLineTable(MCOS->getContext().getDwarfCompileUnitID())
      .getMCLineSections()
      .addLineEntry(LineEntry, Section);
}

//
// This helper routine returns an expression of End - Start + IntVal .
//
static inline const MCExpr *makeEndMinusStartExpr(MCContext &Ctx,
                                                  const MCSymbol &Start,
                                                  const MCSymbol &End,
                                                  int IntVal) {
  MCSymbolRefExpr::VariantKind Variant = MCSymbolRefExpr::VK_None;
  const MCExpr *Res = MCSymbolRefExpr::create(&End, Variant, Ctx);
  const MCExpr *RHS = MCSymbolRefExpr::create(&Start, Variant, Ctx);
  const MCExpr *Res1 = MCBinaryExpr::create(MCBinaryExpr::Sub, Res, RHS, Ctx);
  const MCExpr *Res2 = MCConstantExpr::create(IntVal, Ctx);
  const MCExpr *Res3 = MCBinaryExpr::create(MCBinaryExpr::Sub, Res1, Res2, Ctx);
  return Res3;
}

//
// This helper routine returns an expression of Start + IntVal .
//
static inline const MCExpr *
makeStartPlusIntExpr(MCContext &Ctx, const MCSymbol &Start, int IntVal) {
  MCSymbolRefExpr::VariantKind Variant = MCSymbolRefExpr::VK_None;
  const MCExpr *LHS = MCSymbolRefExpr::create(&Start, Variant, Ctx);
  const MCExpr *RHS = MCConstantExpr::create(IntVal, Ctx);
  const MCExpr *Res = MCBinaryExpr::create(MCBinaryExpr::Add, LHS, RHS, Ctx);
  return Res;
}

//
// This emits the Dwarf line table for the specified section from the entries
// in the LineSection.
//
static inline void emitDwarfLineTable(
    MCStreamer *MCOS, MCSection *Section,
    const MCLineSection::MCDwarfLineEntryCollection &LineEntries) {
  unsigned FileNum = 1;
  unsigned LastLine = 1;
  unsigned Column = 0;
  unsigned Flags = DWARF2_LINE_DEFAULT_IS_STMT ? DWARF2_FLAG_IS_STMT : 0;
  unsigned Isa = 0;
  unsigned Discriminator = 0;
  MCSymbol *LastLabel = nullptr;

  // Loop through each MCDwarfLineEntry and encode the dwarf line number table.
  for (const MCDwarfLineEntry &LineEntry : LineEntries) {
    int64_t LineDelta = static_cast<int64_t>(LineEntry.getLine()) - LastLine;

    if (FileNum != LineEntry.getFileNum()) {
      FileNum = LineEntry.getFileNum();
      MCOS->emitInt8(dwarf::DW_LNS_set_file);
      MCOS->emitULEB128IntValue(FileNum);
    }
    if (Column != LineEntry.getColumn()) {
      Column = LineEntry.getColumn();
      MCOS->emitInt8(dwarf::DW_LNS_set_column);
      MCOS->emitULEB128IntValue(Column);
    }
    if (Discriminator != LineEntry.getDiscriminator() &&
        MCOS->getContext().getDwarfVersion() >= 4) {
      Discriminator = LineEntry.getDiscriminator();
      unsigned Size = getULEB128Size(Discriminator);
      MCOS->emitInt8(dwarf::DW_LNS_extended_op);
      MCOS->emitULEB128IntValue(Size + 1);
      MCOS->emitInt8(dwarf::DW_LNE_set_discriminator);
      MCOS->emitULEB128IntValue(Discriminator);
    }
    if (Isa != LineEntry.getIsa()) {
      Isa = LineEntry.getIsa();
      MCOS->emitInt8(dwarf::DW_LNS_set_isa);
      MCOS->emitULEB128IntValue(Isa);
    }
    if ((LineEntry.getFlags() ^ Flags) & DWARF2_FLAG_IS_STMT) {
      Flags = LineEntry.getFlags();
      MCOS->emitInt8(dwarf::DW_LNS_negate_stmt);
    }
    if (LineEntry.getFlags() & DWARF2_FLAG_BASIC_BLOCK)
      MCOS->emitInt8(dwarf::DW_LNS_set_basic_block);
    if (LineEntry.getFlags() & DWARF2_FLAG_PROLOGUE_END)
      MCOS->emitInt8(dwarf::DW_LNS_set_prologue_end);
    if (LineEntry.getFlags() & DWARF2_FLAG_EPILOGUE_BEGIN)
      MCOS->emitInt8(dwarf::DW_LNS_set_epilogue_begin);

    MCSymbol *Label = LineEntry.getLabel();

    // At this point we want to emit/create the sequence to encode the delta in
    // line numbers and the increment of the address from the previous Label
    // and the current Label.
    const MCAsmInfo *asmInfo = MCOS->getContext().getAsmInfo();
    MCOS->emitDwarfAdvanceLineAddr(LineDelta, LastLabel, Label,
                                   asmInfo->getCodePointerSize());

    Discriminator = 0;
    LastLine = LineEntry.getLine();
    LastLabel = Label;
  }

  // Generate DWARF line end entry.
  MCOS->emitDwarfLineEndEntry(Section, LastLabel);
}

//
// This emits the Dwarf file and the line tables.
//
void MCDwarfLineTable::emit(MCStreamer *MCOS, MCDwarfLineTableParams Params) {
  MCContext &context = MCOS->getContext();

  auto &LineTables = context.getMCDwarfLineTables();

  // Bail out early so we don't switch to the debug_line section needlessly and
  // in doing so create an unnecessary (if empty) section.
  if (LineTables.empty())
    return;

  // In a v5 non-split line table, put the strings in a separate section.
  Optional<MCDwarfLineStr> LineStr;
  if (context.getDwarfVersion() >= 5)
    LineStr = MCDwarfLineStr(context);

  // Switch to the section where the table will be emitted into.
  MCOS->SwitchSection(context.getObjectFileInfo()->getDwarfLineSection());

  // Handle the rest of the Compile Units.
  for (const auto &CUIDTablePair : LineTables) {
    CUIDTablePair.second.emitCU(MCOS, Params, LineStr);
  }

  if (LineStr)
    LineStr->emitSection(MCOS);
}

void MCDwarfDwoLineTable::Emit(MCStreamer &MCOS, MCDwarfLineTableParams Params,
                               MCSection *Section) const {
  if (!HasSplitLineTable)
    return;
  Optional<MCDwarfLineStr> NoLineStr(None);
  MCOS.SwitchSection(Section);
  MCOS.emitLabel(Header.Emit(&MCOS, Params, None, NoLineStr).second);
}

std::pair<MCSymbol *, MCSymbol *>
MCDwarfLineTableHeader::Emit(MCStreamer *MCOS, MCDwarfLineTableParams Params,
                             Optional<MCDwarfLineStr> &LineStr) const {
  static const char StandardOpcodeLengths[] = {
      0, // length of DW_LNS_copy
      1, // length of DW_LNS_advance_pc
      1, // length of DW_LNS_advance_line
      1, // length of DW_LNS_set_file
      1, // length of DW_LNS_set_column
      0, // length of DW_LNS_negate_stmt
      0, // length of DW_LNS_set_basic_block
      0, // length of DW_LNS_const_add_pc
      1, // length of DW_LNS_fixed_advance_pc
      0, // length of DW_LNS_set_prologue_end
      0, // length of DW_LNS_set_epilogue_begin
      1  // DW_LNS_set_isa
  };
  assert(array_lengthof(StandardOpcodeLengths) >=
         (Params.DWARF2LineOpcodeBase - 1U));
  return Emit(
      MCOS, Params,
      makeArrayRef(StandardOpcodeLengths, Params.DWARF2LineOpcodeBase - 1),
      LineStr);
}

static const MCExpr *forceExpAbs(MCStreamer &OS, const MCExpr* Expr) {
  MCContext &Context = OS.getContext();
  assert(!isa<MCSymbolRefExpr>(Expr));
  if (Context.getAsmInfo()->hasAggressiveSymbolFolding())
    return Expr;

  MCSymbol *ABS = Context.createTempSymbol();
  OS.emitAssignment(ABS, Expr);
  return MCSymbolRefExpr::create(ABS, Context);
}

static void emitAbsValue(MCStreamer &OS, const MCExpr *Value, unsigned Size) {
  const MCExpr *ABS = forceExpAbs(OS, Value);
  OS.emitValue(ABS, Size);
}

void MCDwarfLineStr::emitSection(MCStreamer *MCOS) {
  // Switch to the .debug_line_str section.
  MCOS->SwitchSection(
      MCOS->getContext().getObjectFileInfo()->getDwarfLineStrSection());
  // Emit the strings without perturbing the offsets we used.
  LineStrings.finalizeInOrder();
  SmallString<0> Data;
  Data.resize(LineStrings.getSize());
  LineStrings.write((uint8_t *)Data.data());
  MCOS->emitBinaryData(Data.str());
}

void MCDwarfLineStr::emitRef(MCStreamer *MCOS, StringRef Path) {
  int RefSize =
      dwarf::getDwarfOffsetByteSize(MCOS->getContext().getDwarfFormat());
  size_t Offset = LineStrings.add(Path);
  if (UseRelocs) {
    MCContext &Ctx = MCOS->getContext();
    MCOS->emitValue(makeStartPlusIntExpr(Ctx, *LineStrLabel, Offset), RefSize);
  } else
    MCOS->emitIntValue(Offset, RefSize);
}

void MCDwarfLineTableHeader::emitV2FileDirTables(MCStreamer *MCOS) const {
  // First the directory table.
  for (auto &Dir : MCDwarfDirs) {
    MCOS->emitBytes(Dir);                // The DirectoryName, and...
    MCOS->emitBytes(StringRef("\0", 1)); // its null terminator.
  }
  MCOS->emitInt8(0); // Terminate the directory list.

  // Second the file table.
  for (unsigned i = 1; i < MCDwarfFiles.size(); i++) {
    assert(!MCDwarfFiles[i].Name.empty());
    MCOS->emitBytes(MCDwarfFiles[i].Name); // FileName and...
    MCOS->emitBytes(StringRef("\0", 1));   // its null terminator.
    MCOS->emitULEB128IntValue(MCDwarfFiles[i].DirIndex); // Directory number.
    MCOS->emitInt8(0); // Last modification timestamp (always 0).
    MCOS->emitInt8(0); // File size (always 0).
  }
  MCOS->emitInt8(0); // Terminate the file list.
}

static void emitOneV5FileEntry(MCStreamer *MCOS, const MCDwarfFile &DwarfFile,
                               bool EmitMD5, bool HasSource,
                               Optional<MCDwarfLineStr> &LineStr) {
  assert(!DwarfFile.Name.empty());
  if (LineStr)
    LineStr->emitRef(MCOS, DwarfFile.Name);
  else {
    MCOS->emitBytes(DwarfFile.Name);     // FileName and...
    MCOS->emitBytes(StringRef("\0", 1)); // its null terminator.
  }
  MCOS->emitULEB128IntValue(DwarfFile.DirIndex); // Directory number.
  if (EmitMD5) {
    const MD5::MD5Result &Cksum = *DwarfFile.Checksum;
    MCOS->emitBinaryData(
        StringRef(reinterpret_cast<const char *>(Cksum.Bytes.data()),
                  Cksum.Bytes.size()));
  }
  if (HasSource) {
    if (LineStr)
      LineStr->emitRef(MCOS, DwarfFile.Source.getValueOr(StringRef()));
    else {
      MCOS->emitBytes(
          DwarfFile.Source.getValueOr(StringRef())); // Source and...
      MCOS->emitBytes(StringRef("\0", 1));           // its null terminator.
    }
  }
}

void MCDwarfLineTableHeader::emitV5FileDirTables(
    MCStreamer *MCOS, Optional<MCDwarfLineStr> &LineStr) const {
  // The directory format, which is just a list of the directory paths.  In a
  // non-split object, these are references to .debug_line_str; in a split
  // object, they are inline strings.
  MCOS->emitInt8(1);
  MCOS->emitULEB128IntValue(dwarf::DW_LNCT_path);
  MCOS->emitULEB128IntValue(LineStr ? dwarf::DW_FORM_line_strp
                                    : dwarf::DW_FORM_string);
  MCOS->emitULEB128IntValue(MCDwarfDirs.size() + 1);
  // Try not to emit an empty compilation directory.
  const StringRef CompDir = CompilationDir.empty()
                                ? MCOS->getContext().getCompilationDir()
                                : StringRef(CompilationDir);
  if (LineStr) {
    // Record path strings, emit references here.
    LineStr->emitRef(MCOS, CompDir);
    for (const auto &Dir : MCDwarfDirs)
      LineStr->emitRef(MCOS, Dir);
  } else {
    // The list of directory paths.  Compilation directory comes first.
    MCOS->emitBytes(CompDir);
    MCOS->emitBytes(StringRef("\0", 1));
    for (const auto &Dir : MCDwarfDirs) {
      MCOS->emitBytes(Dir);                // The DirectoryName, and...
      MCOS->emitBytes(StringRef("\0", 1)); // its null terminator.
    }
  }

  // The file format, which is the inline null-terminated filename and a
  // directory index.  We don't track file size/timestamp so don't emit them
  // in the v5 table.  Emit MD5 checksums and source if we have them.
  uint64_t Entries = 2;
  if (HasAllMD5)
    Entries += 1;
  if (HasSource)
    Entries += 1;
  MCOS->emitInt8(Entries);
  MCOS->emitULEB128IntValue(dwarf::DW_LNCT_path);
  MCOS->emitULEB128IntValue(LineStr ? dwarf::DW_FORM_line_strp
                                    : dwarf::DW_FORM_string);
  MCOS->emitULEB128IntValue(dwarf::DW_LNCT_directory_index);
  MCOS->emitULEB128IntValue(dwarf::DW_FORM_udata);
  if (HasAllMD5) {
    MCOS->emitULEB128IntValue(dwarf::DW_LNCT_MD5);
    MCOS->emitULEB128IntValue(dwarf::DW_FORM_data16);
  }
  if (HasSource) {
    MCOS->emitULEB128IntValue(dwarf::DW_LNCT_LLVM_source);
    MCOS->emitULEB128IntValue(LineStr ? dwarf::DW_FORM_line_strp
                                      : dwarf::DW_FORM_string);
  }
  // Then the counted list of files. The root file is file #0, then emit the
  // files as provide by .file directives.
  // MCDwarfFiles has an unused element [0] so use size() not size()+1.
  // But sometimes MCDwarfFiles is empty, in which case we still emit one file.
  MCOS->emitULEB128IntValue(MCDwarfFiles.empty() ? 1 : MCDwarfFiles.size());
  // To accommodate assembler source written for DWARF v4 but trying to emit
  // v5: If we didn't see a root file explicitly, replicate file #1.
  assert((!RootFile.Name.empty() || MCDwarfFiles.size() >= 1) &&
         "No root file and no .file directives");
  emitOneV5FileEntry(MCOS, RootFile.Name.empty() ? MCDwarfFiles[1] : RootFile,
                     HasAllMD5, HasSource, LineStr);
  for (unsigned i = 1; i < MCDwarfFiles.size(); ++i)
    emitOneV5FileEntry(MCOS, MCDwarfFiles[i], HasAllMD5, HasSource, LineStr);
}

std::pair<MCSymbol *, MCSymbol *>
MCDwarfLineTableHeader::Emit(MCStreamer *MCOS, MCDwarfLineTableParams Params,
                             ArrayRef<char> StandardOpcodeLengths,
                             Optional<MCDwarfLineStr> &LineStr) const {
  MCContext &context = MCOS->getContext();

  // Create a symbol at the beginning of the line table.
  MCSymbol *LineStartSym = Label;
  if (!LineStartSym)
    LineStartSym = context.createTempSymbol();

  // Set the value of the symbol, as we are at the start of the line table.
  MCOS->emitDwarfLineStartLabel(LineStartSym);

  unsigned OffsetSize = dwarf::getDwarfOffsetByteSize(context.getDwarfFormat());

  MCSymbol *LineEndSym = MCOS->emitDwarfUnitLength("debug_line", "unit length");

  // Next 2 bytes is the Version.
  unsigned LineTableVersion = context.getDwarfVersion();
  MCOS->emitInt16(LineTableVersion);

  // In v5, we get address info next.
  if (LineTableVersion >= 5) {
    MCOS->emitInt8(context.getAsmInfo()->getCodePointerSize());
    MCOS->emitInt8(0); // Segment selector; same as EmitGenDwarfAranges.
  }

  // Create symbols for the start/end of the prologue.
  MCSymbol *ProStartSym = context.createTempSymbol("prologue_start");
  MCSymbol *ProEndSym = context.createTempSymbol("prologue_end");

  // Length of the prologue, is the next 4 bytes (8 bytes for DWARF64). This is
  // actually the length from after the length word, to the end of the prologue.
  MCOS->emitAbsoluteSymbolDiff(ProEndSym, ProStartSym, OffsetSize);

  MCOS->emitLabel(ProStartSym);

  // Parameters of the state machine, are next.
  MCOS->emitInt8(context.getAsmInfo()->getMinInstAlignment());
  // maximum_operations_per_instruction
  // For non-VLIW architectures this field is always 1.
  // FIXME: VLIW architectures need to update this field accordingly.
  if (LineTableVersion >= 4)
    MCOS->emitInt8(1);
  MCOS->emitInt8(DWARF2_LINE_DEFAULT_IS_STMT);
  MCOS->emitInt8(Params.DWARF2LineBase);
  MCOS->emitInt8(Params.DWARF2LineRange);
  MCOS->emitInt8(StandardOpcodeLengths.size() + 1);

  // Standard opcode lengths
  for (char Length : StandardOpcodeLengths)
    MCOS->emitInt8(Length);

  // Put out the directory and file tables.  The formats vary depending on
  // the version.
  if (LineTableVersion >= 5)
    emitV5FileDirTables(MCOS, LineStr);
  else
    emitV2FileDirTables(MCOS);

  // This is the end of the prologue, so set the value of the symbol at the
  // end of the prologue (that was used in a previous expression).
  MCOS->emitLabel(ProEndSym);

  return std::make_pair(LineStartSym, LineEndSym);
}

void MCDwarfLineTable::emitCU(MCStreamer *MCOS, MCDwarfLineTableParams Params,
                              Optional<MCDwarfLineStr> &LineStr) const {
  MCSymbol *LineEndSym = Header.Emit(MCOS, Params, LineStr).second;

  // Put out the line tables.
  for (const auto &LineSec : MCLineSections.getMCLineEntries())
    emitDwarfLineTable(MCOS, LineSec.first, LineSec.second);

  // This is the end of the section, so set the value of the symbol at the end
  // of this section (that was used in a previous expression).
  MCOS->emitLabel(LineEndSym);
}

Expected<unsigned> MCDwarfLineTable::tryGetFile(StringRef &Directory,
                                                StringRef &FileName,
                                                Optional<MD5::MD5Result> Checksum,
                                                Optional<StringRef> Source,
                                                uint16_t DwarfVersion,
                                                unsigned FileNumber) {
  return Header.tryGetFile(Directory, FileName, Checksum, Source, DwarfVersion,
                           FileNumber);
}

static bool isRootFile(const MCDwarfFile &RootFile, StringRef &Directory,
                       StringRef &FileName, Optional<MD5::MD5Result> Checksum) {
  if (RootFile.Name.empty() || RootFile.Name != FileName.data())
    return false;
  return RootFile.Checksum == Checksum;
}

Expected<unsigned>
MCDwarfLineTableHeader::tryGetFile(StringRef &Directory,
                                   StringRef &FileName,
                                   Optional<MD5::MD5Result> Checksum,
                                   Optional<StringRef> Source,
                                   uint16_t DwarfVersion,
                                   unsigned FileNumber) {
  if (Directory == CompilationDir)
    Directory = "";
  if (FileName.empty()) {
    FileName = "<stdin>";
    Directory = "";
  }
  assert(!FileName.empty());
  // Keep track of whether any or all files have an MD5 checksum.
  // If any files have embedded source, they all must.
  if (MCDwarfFiles.empty()) {
    trackMD5Usage(Checksum.hasValue());
    HasSource = (Source != None);
  }
  if (isRootFile(RootFile, Directory, FileName, Checksum) && DwarfVersion >= 5)
    return 0;
  if (FileNumber == 0) {
    // File numbers start with 1 and/or after any file numbers
    // allocated by inline-assembler .file directives.
    FileNumber = MCDwarfFiles.empty() ? 1 : MCDwarfFiles.size();
    SmallString<256> Buffer;
    auto IterBool = SourceIdMap.insert(
        std::make_pair((Directory + Twine('\0') + FileName).toStringRef(Buffer),
                       FileNumber));
    if (!IterBool.second)
      return IterBool.first->second;
  }
  // Make space for this FileNumber in the MCDwarfFiles vector if needed.
  if (FileNumber >= MCDwarfFiles.size())
    MCDwarfFiles.resize(FileNumber + 1);

  // Get the new MCDwarfFile slot for this FileNumber.
  MCDwarfFile &File = MCDwarfFiles[FileNumber];

  // It is an error to see the same number more than once.
  if (!File.Name.empty())
    return make_error<StringError>("file number already allocated",
                                   inconvertibleErrorCode());

  // If any files have embedded source, they all must.
  if (HasSource != (Source != None))
    return make_error<StringError>("inconsistent use of embedded source",
                                   inconvertibleErrorCode());

  if (Directory.empty()) {
    // Separate the directory part from the basename of the FileName.
    StringRef tFileName = sys::path::filename(FileName);
    if (!tFileName.empty()) {
      Directory = sys::path::parent_path(FileName);
      if (!Directory.empty())
        FileName = tFileName;
    }
  }

  // Find or make an entry in the MCDwarfDirs vector for this Directory.
  // Capture directory name.
  unsigned DirIndex;
  if (Directory.empty()) {
    // For FileNames with no directories a DirIndex of 0 is used.
    DirIndex = 0;
  } else {
    DirIndex = llvm::find(MCDwarfDirs, Directory) - MCDwarfDirs.begin();
    if (DirIndex >= MCDwarfDirs.size())
      MCDwarfDirs.push_back(std::string(Directory));
    // The DirIndex is one based, as DirIndex of 0 is used for FileNames with
    // no directories.  MCDwarfDirs[] is unlike MCDwarfFiles[] in that the
    // directory names are stored at MCDwarfDirs[DirIndex-1] where FileNames
    // are stored at MCDwarfFiles[FileNumber].Name .
    DirIndex++;
  }

  File.Name = std::string(FileName);
  File.DirIndex = DirIndex;
  File.Checksum = Checksum;
  trackMD5Usage(Checksum.hasValue());
  File.Source = Source;
  if (Source)
    HasSource = true;

  // return the allocated FileNumber.
  return FileNumber;
}

/// Utility function to emit the encoding to a streamer.
void MCDwarfLineAddr::Emit(MCStreamer *MCOS, MCDwarfLineTableParams Params,
                           int64_t LineDelta, uint64_t AddrDelta) {
  MCContext &Context = MCOS->getContext();
  SmallString<256> Tmp;
  raw_svector_ostream OS(Tmp);
  MCDwarfLineAddr::Encode(Context, Params, LineDelta, AddrDelta, OS);
  MCOS->emitBytes(OS.str());
}

/// Given a special op, return the address skip amount (in units of
/// DWARF2_LINE_MIN_INSN_LENGTH).
static uint64_t SpecialAddr(MCDwarfLineTableParams Params, uint64_t op) {
  return (op - Params.DWARF2LineOpcodeBase) / Params.DWARF2LineRange;
}

/// Utility function to encode a Dwarf pair of LineDelta and AddrDeltas.
void MCDwarfLineAddr::Encode(MCContext &Context, MCDwarfLineTableParams Params,
                             int64_t LineDelta, uint64_t AddrDelta,
                             raw_ostream &OS) {
  uint64_t Temp, Opcode;
  bool NeedCopy = false;

  // The maximum address skip amount that can be encoded with a special op.
  uint64_t MaxSpecialAddrDelta = SpecialAddr(Params, 255);

  // Scale the address delta by the minimum instruction length.
  AddrDelta = ScaleAddrDelta(Context, AddrDelta);

  // A LineDelta of INT64_MAX is a signal that this is actually a
  // DW_LNE_end_sequence. We cannot use special opcodes here, since we want the
  // end_sequence to emit the matrix entry.
  if (LineDelta == INT64_MAX) {
    if (AddrDelta == MaxSpecialAddrDelta)
      OS << char(dwarf::DW_LNS_const_add_pc);
    else if (AddrDelta) {
      OS << char(dwarf::DW_LNS_advance_pc);
      encodeULEB128(AddrDelta, OS);
    }
    OS << char(dwarf::DW_LNS_extended_op);
    OS << char(1);
    OS << char(dwarf::DW_LNE_end_sequence);
    return;
  }

  // Bias the line delta by the base.
  Temp = LineDelta - Params.DWARF2LineBase;

  // If the line increment is out of range of a special opcode, we must encode
  // it with DW_LNS_advance_line.
  if (Temp >= Params.DWARF2LineRange ||
      Temp + Params.DWARF2LineOpcodeBase > 255) {
    OS << char(dwarf::DW_LNS_advance_line);
    encodeSLEB128(LineDelta, OS);

    LineDelta = 0;
    Temp = 0 - Params.DWARF2LineBase;
    NeedCopy = true;
  }

  // Use DW_LNS_copy instead of a "line +0, addr +0" special opcode.
  if (LineDelta == 0 && AddrDelta == 0) {
    OS << char(dwarf::DW_LNS_copy);
    return;
  }

  // Bias the opcode by the special opcode base.
  Temp += Params.DWARF2LineOpcodeBase;

  // Avoid overflow when addr_delta is large.
  if (AddrDelta < 256 + MaxSpecialAddrDelta) {
    // Try using a special opcode.
    Opcode = Temp + AddrDelta * Params.DWARF2LineRange;
    if (Opcode <= 255) {
      OS << char(Opcode);
      return;
    }

    // Try using DW_LNS_const_add_pc followed by special op.
    Opcode = Temp + (AddrDelta - MaxSpecialAddrDelta) * Params.DWARF2LineRange;
    if (Opcode <= 255) {
      OS << char(dwarf::DW_LNS_const_add_pc);
      OS << char(Opcode);
      return;
    }
  }

  // Otherwise use DW_LNS_advance_pc.
  OS << char(dwarf::DW_LNS_advance_pc);
  encodeULEB128(AddrDelta, OS);

  if (NeedCopy)
    OS << char(dwarf::DW_LNS_copy);
  else {
    assert(Temp <= 255 && "Buggy special opcode encoding.");
    OS << char(Temp);
  }
}

std::tuple<uint32_t, uint32_t, bool>
MCDwarfLineAddr::fixedEncode(MCContext &Context, int64_t LineDelta,
                             uint64_t AddrDelta, raw_ostream &OS) {
  uint32_t Offset, Size;
  if (LineDelta != INT64_MAX) {
    OS << char(dwarf::DW_LNS_advance_line);
    encodeSLEB128(LineDelta, OS);
  }

  // Use address delta to adjust address or use absolute address to adjust
  // address.
  bool SetDelta;
  // According to DWARF spec., the DW_LNS_fixed_advance_pc opcode takes a
  // single uhalf (unencoded) operand. So, the maximum value of AddrDelta
  // is 65535. We set a conservative upper bound for it for relaxation.
  if (AddrDelta > 60000) {
    const MCAsmInfo *asmInfo = Context.getAsmInfo();
    unsigned AddrSize = asmInfo->getCodePointerSize();

    OS << char(dwarf::DW_LNS_extended_op);
    encodeULEB128(1 + AddrSize, OS);
    OS << char(dwarf::DW_LNE_set_address);
    // Generate fixup for the address.
    Offset = OS.tell();
    Size = AddrSize;
    SetDelta = false;
    OS.write_zeros(AddrSize);
  } else {
    OS << char(dwarf::DW_LNS_fixed_advance_pc);
    // Generate fixup for 2-bytes address delta.
    Offset = OS.tell();
    Size = 2;
    SetDelta = true;
    OS << char(0);
    OS << char(0);
  }

  if (LineDelta == INT64_MAX) {
    OS << char(dwarf::DW_LNS_extended_op);
    OS << char(1);
    OS << char(dwarf::DW_LNE_end_sequence);
  } else {
    OS << char(dwarf::DW_LNS_copy);
  }

  return std::make_tuple(Offset, Size, SetDelta);
}

// Utility function to write a tuple for .debug_abbrev.
static void EmitAbbrev(MCStreamer *MCOS, uint64_t Name, uint64_t Form) {
  MCOS->emitULEB128IntValue(Name);
  MCOS->emitULEB128IntValue(Form);
}

// When generating dwarf for assembly source files this emits
// the data for .debug_abbrev section which contains three DIEs.
static void EmitGenDwarfAbbrev(MCStreamer *MCOS) {
  MCContext &context = MCOS->getContext();
  MCOS->SwitchSection(context.getObjectFileInfo()->getDwarfAbbrevSection());

  // DW_TAG_compile_unit DIE abbrev (1).
  MCOS->emitULEB128IntValue(1);
  MCOS->emitULEB128IntValue(dwarf::DW_TAG_compile_unit);
  MCOS->emitInt8(dwarf::DW_CHILDREN_yes);
  dwarf::Form SecOffsetForm =
      context.getDwarfVersion() >= 4
          ? dwarf::DW_FORM_sec_offset
          : (context.getDwarfFormat() == dwarf::DWARF64 ? dwarf::DW_FORM_data8
                                                        : dwarf::DW_FORM_data4);
  EmitAbbrev(MCOS, dwarf::DW_AT_stmt_list, SecOffsetForm);
  if (context.getGenDwarfSectionSyms().size() > 1 &&
      context.getDwarfVersion() >= 3) {
    EmitAbbrev(MCOS, dwarf::DW_AT_ranges, SecOffsetForm);
  } else {
    EmitAbbrev(MCOS, dwarf::DW_AT_low_pc, dwarf::DW_FORM_addr);
    EmitAbbrev(MCOS, dwarf::DW_AT_high_pc, dwarf::DW_FORM_addr);
  }
  EmitAbbrev(MCOS, dwarf::DW_AT_name, dwarf::DW_FORM_string);
  if (!context.getCompilationDir().empty())
    EmitAbbrev(MCOS, dwarf::DW_AT_comp_dir, dwarf::DW_FORM_string);
  StringRef DwarfDebugFlags = context.getDwarfDebugFlags();
  if (!DwarfDebugFlags.empty())
    EmitAbbrev(MCOS, dwarf::DW_AT_APPLE_flags, dwarf::DW_FORM_string);
  EmitAbbrev(MCOS, dwarf::DW_AT_producer, dwarf::DW_FORM_string);
  EmitAbbrev(MCOS, dwarf::DW_AT_language, dwarf::DW_FORM_data2);
  EmitAbbrev(MCOS, 0, 0);

  // DW_TAG_label DIE abbrev (2).
  MCOS->emitULEB128IntValue(2);
  MCOS->emitULEB128IntValue(dwarf::DW_TAG_label);
  MCOS->emitInt8(dwarf::DW_CHILDREN_no);
  EmitAbbrev(MCOS, dwarf::DW_AT_name, dwarf::DW_FORM_string);
  EmitAbbrev(MCOS, dwarf::DW_AT_decl_file, dwarf::DW_FORM_data4);
  EmitAbbrev(MCOS, dwarf::DW_AT_decl_line, dwarf::DW_FORM_data4);
  EmitAbbrev(MCOS, dwarf::DW_AT_low_pc, dwarf::DW_FORM_addr);
  EmitAbbrev(MCOS, 0, 0);

  // Terminate the abbreviations for this compilation unit.
  MCOS->emitInt8(0);
}

// When generating dwarf for assembly source files this emits the data for
// .debug_aranges section. This section contains a header and a table of pairs
// of PointerSize'ed values for the address and size of section(s) with line
// table entries.
static void EmitGenDwarfAranges(MCStreamer *MCOS,
                                const MCSymbol *InfoSectionSymbol) {
  MCContext &context = MCOS->getContext();

  auto &Sections = context.getGenDwarfSectionSyms();

  MCOS->SwitchSection(context.getObjectFileInfo()->getDwarfARangesSection());

  unsigned UnitLengthBytes =
      dwarf::getUnitLengthFieldByteSize(context.getDwarfFormat());
  unsigned OffsetSize = dwarf::getDwarfOffsetByteSize(context.getDwarfFormat());

  // This will be the length of the .debug_aranges section, first account for
  // the size of each item in the header (see below where we emit these items).
  int Length = UnitLengthBytes + 2 + OffsetSize + 1 + 1;

  // Figure the padding after the header before the table of address and size
  // pairs who's values are PointerSize'ed.
  const MCAsmInfo *asmInfo = context.getAsmInfo();
  int AddrSize = asmInfo->getCodePointerSize();
  int Pad = 2 * AddrSize - (Length & (2 * AddrSize - 1));
  if (Pad == 2 * AddrSize)
    Pad = 0;
  Length += Pad;

  // Add the size of the pair of PointerSize'ed values for the address and size
  // of each section we have in the table.
  Length += 2 * AddrSize * Sections.size();
  // And the pair of terminating zeros.
  Length += 2 * AddrSize;

  // Emit the header for this section.
  if (context.getDwarfFormat() == dwarf::DWARF64)
    // The DWARF64 mark.
    MCOS->emitInt32(dwarf::DW_LENGTH_DWARF64);
  // The 4 (8 for DWARF64) byte length not including the length of the unit
  // length field itself.
  MCOS->emitIntValue(Length - UnitLengthBytes, OffsetSize);
  // The 2 byte version, which is 2.
  MCOS->emitInt16(2);
  // The 4 (8 for DWARF64) byte offset to the compile unit in the .debug_info
  // from the start of the .debug_info.
  if (InfoSectionSymbol)
    MCOS->emitSymbolValue(InfoSectionSymbol, OffsetSize,
                          asmInfo->needsDwarfSectionOffsetDirective());
  else
    MCOS->emitIntValue(0, OffsetSize);
  // The 1 byte size of an address.
  MCOS->emitInt8(AddrSize);
  // The 1 byte size of a segment descriptor, we use a value of zero.
  MCOS->emitInt8(0);
  // Align the header with the padding if needed, before we put out the table.
  for(int i = 0; i < Pad; i++)
    MCOS->emitInt8(0);

  // Now emit the table of pairs of PointerSize'ed values for the section
  // addresses and sizes.
  for (MCSection *Sec : Sections) {
    const MCSymbol *StartSymbol = Sec->getBeginSymbol();
    MCSymbol *EndSymbol = Sec->getEndSymbol(context);
    assert(StartSymbol && "StartSymbol must not be NULL");
    assert(EndSymbol && "EndSymbol must not be NULL");

    const MCExpr *Addr = MCSymbolRefExpr::create(
      StartSymbol, MCSymbolRefExpr::VK_None, context);
    const MCExpr *Size =
        makeEndMinusStartExpr(context, *StartSymbol, *EndSymbol, 0);
    MCOS->emitValue(Addr, AddrSize);
    emitAbsValue(*MCOS, Size, AddrSize);
  }

  // And finally the pair of terminating zeros.
  MCOS->emitIntValue(0, AddrSize);
  MCOS->emitIntValue(0, AddrSize);
}

// When generating dwarf for assembly source files this emits the data for
// .debug_info section which contains three parts.  The header, the compile_unit
// DIE and a list of label DIEs.
static void EmitGenDwarfInfo(MCStreamer *MCOS,
                             const MCSymbol *AbbrevSectionSymbol,
                             const MCSymbol *LineSectionSymbol,
                             const MCSymbol *RangesSymbol) {
  MCContext &context = MCOS->getContext();

  MCOS->SwitchSection(context.getObjectFileInfo()->getDwarfInfoSection());

  // Create a symbol at the start and end of this section used in here for the
  // expression to calculate the length in the header.
  MCSymbol *InfoStart = context.createTempSymbol();
  MCOS->emitLabel(InfoStart);
  MCSymbol *InfoEnd = context.createTempSymbol();

  // First part: the header.

  unsigned UnitLengthBytes =
      dwarf::getUnitLengthFieldByteSize(context.getDwarfFormat());
  unsigned OffsetSize = dwarf::getDwarfOffsetByteSize(context.getDwarfFormat());

  if (context.getDwarfFormat() == dwarf::DWARF64)
    // Emit DWARF64 mark.
    MCOS->emitInt32(dwarf::DW_LENGTH_DWARF64);

  // The 4 (8 for DWARF64) byte total length of the information for this
  // compilation unit, not including the unit length field itself.
  const MCExpr *Length =
      makeEndMinusStartExpr(context, *InfoStart, *InfoEnd, UnitLengthBytes);
  emitAbsValue(*MCOS, Length, OffsetSize);

  // The 2 byte DWARF version.
  MCOS->emitInt16(context.getDwarfVersion());

  // The DWARF v5 header has unit type, address size, abbrev offset.
  // Earlier versions have abbrev offset, address size.
  const MCAsmInfo &AsmInfo = *context.getAsmInfo();
  int AddrSize = AsmInfo.getCodePointerSize();
  if (context.getDwarfVersion() >= 5) {
    MCOS->emitInt8(dwarf::DW_UT_compile);
    MCOS->emitInt8(AddrSize);
  }
  // The 4 (8 for DWARF64) byte offset to the debug abbrevs from the start of
  // the .debug_abbrev.
  if (AbbrevSectionSymbol)
    MCOS->emitSymbolValue(AbbrevSectionSymbol, OffsetSize,
                          AsmInfo.needsDwarfSectionOffsetDirective());
  else
    // Since the abbrevs are at the start of the section, the offset is zero.
    MCOS->emitIntValue(0, OffsetSize);
  if (context.getDwarfVersion() <= 4)
    MCOS->emitInt8(AddrSize);

  // Second part: the compile_unit DIE.

  // The DW_TAG_compile_unit DIE abbrev (1).
  MCOS->emitULEB128IntValue(1);

  // DW_AT_stmt_list, a 4 (8 for DWARF64) byte offset from the start of the
  // .debug_line section.
  if (LineSectionSymbol)
    MCOS->emitSymbolValue(LineSectionSymbol, OffsetSize,
                          AsmInfo.needsDwarfSectionOffsetDirective());
  else
    // The line table is at the start of the section, so the offset is zero.
    MCOS->emitIntValue(0, OffsetSize);

  if (RangesSymbol) {
    // There are multiple sections containing code, so we must use
    // .debug_ranges/.debug_rnglists. AT_ranges, the 4/8 byte offset from the
    // start of the .debug_ranges/.debug_rnglists.
    MCOS->emitSymbolValue(RangesSymbol, OffsetSize);
  } else {
    // If we only have one non-empty code section, we can use the simpler
    // AT_low_pc and AT_high_pc attributes.

    // Find the first (and only) non-empty text section
    auto &Sections = context.getGenDwarfSectionSyms();
    const auto TextSection = Sections.begin();
    assert(TextSection != Sections.end() && "No text section found");

    MCSymbol *StartSymbol = (*TextSection)->getBeginSymbol();
    MCSymbol *EndSymbol = (*TextSection)->getEndSymbol(context);
    assert(StartSymbol && "StartSymbol must not be NULL");
    assert(EndSymbol && "EndSymbol must not be NULL");

    // AT_low_pc, the first address of the default .text section.
    const MCExpr *Start = MCSymbolRefExpr::create(
        StartSymbol, MCSymbolRefExpr::VK_None, context);
    MCOS->emitValue(Start, AddrSize);

    // AT_high_pc, the last address of the default .text section.
    const MCExpr *End = MCSymbolRefExpr::create(
      EndSymbol, MCSymbolRefExpr::VK_None, context);
    MCOS->emitValue(End, AddrSize);
  }

  // AT_name, the name of the source file.  Reconstruct from the first directory
  // and file table entries.
  const SmallVectorImpl<std::string> &MCDwarfDirs = context.getMCDwarfDirs();
  if (MCDwarfDirs.size() > 0) {
    MCOS->emitBytes(MCDwarfDirs[0]);
    MCOS->emitBytes(sys::path::get_separator());
  }
  const SmallVectorImpl<MCDwarfFile> &MCDwarfFiles = context.getMCDwarfFiles();
  // MCDwarfFiles might be empty if we have an empty source file.
  // If it's not empty, [0] is unused and [1] is the first actual file.
  assert(MCDwarfFiles.empty() || MCDwarfFiles.size() >= 2);
  const MCDwarfFile &RootFile =
      MCDwarfFiles.empty()
          ? context.getMCDwarfLineTable(/*CUID=*/0).getRootFile()
          : MCDwarfFiles[1];
  MCOS->emitBytes(RootFile.Name);
  MCOS->emitInt8(0); // NULL byte to terminate the string.

  // AT_comp_dir, the working directory the assembly was done in.
  if (!context.getCompilationDir().empty()) {
    MCOS->emitBytes(context.getCompilationDir());
    MCOS->emitInt8(0); // NULL byte to terminate the string.
  }

  // AT_APPLE_flags, the command line arguments of the assembler tool.
  StringRef DwarfDebugFlags = context.getDwarfDebugFlags();
  if (!DwarfDebugFlags.empty()){
    MCOS->emitBytes(DwarfDebugFlags);
    MCOS->emitInt8(0); // NULL byte to terminate the string.
  }

  // AT_producer, the version of the assembler tool.
  StringRef DwarfDebugProducer = context.getDwarfDebugProducer();
  if (!DwarfDebugProducer.empty())
    MCOS->emitBytes(DwarfDebugProducer);
  else
    MCOS->emitBytes(StringRef("llvm-mc (based on LLVM " PACKAGE_VERSION ")"));
  MCOS->emitInt8(0); // NULL byte to terminate the string.

  // AT_language, a 4 byte value.  We use DW_LANG_Mips_Assembler as the dwarf2
  // draft has no standard code for assembler.
  MCOS->emitInt16(dwarf::DW_LANG_Mips_Assembler);

  // Third part: the list of label DIEs.

  // Loop on saved info for dwarf labels and create the DIEs for them.
  const std::vector<MCGenDwarfLabelEntry> &Entries =
      MCOS->getContext().getMCGenDwarfLabelEntries();
  for (const auto &Entry : Entries) {
    // The DW_TAG_label DIE abbrev (2).
    MCOS->emitULEB128IntValue(2);

    // AT_name, of the label without any leading underbar.
    MCOS->emitBytes(Entry.getName());
    MCOS->emitInt8(0); // NULL byte to terminate the string.

    // AT_decl_file, index into the file table.
    MCOS->emitInt32(Entry.getFileNumber());

    // AT_decl_line, source line number.
    MCOS->emitInt32(Entry.getLineNumber());

    // AT_low_pc, start address of the label.
    const MCExpr *AT_low_pc = MCSymbolRefExpr::create(Entry.getLabel(),
                                             MCSymbolRefExpr::VK_None, context);
    MCOS->emitValue(AT_low_pc, AddrSize);
  }

  // Add the NULL DIE terminating the Compile Unit DIE's.
  MCOS->emitInt8(0);

  // Now set the value of the symbol at the end of the info section.
  MCOS->emitLabel(InfoEnd);
}

// When generating dwarf for assembly source files this emits the data for
// .debug_ranges section. We only emit one range list, which spans all of the
// executable sections of this file.
static MCSymbol *emitGenDwarfRanges(MCStreamer *MCOS) {
  MCContext &context = MCOS->getContext();
  auto &Sections = context.getGenDwarfSectionSyms();

  const MCAsmInfo *AsmInfo = context.getAsmInfo();
  int AddrSize = AsmInfo->getCodePointerSize();
  MCSymbol *RangesSymbol;

  if (MCOS->getContext().getDwarfVersion() >= 5) {
    MCOS->SwitchSection(context.getObjectFileInfo()->getDwarfRnglistsSection());
    MCSymbol *EndSymbol = mcdwarf::emitListsTableHeaderStart(*MCOS);
    MCOS->AddComment("Offset entry count");
    MCOS->emitInt32(0);
    RangesSymbol = context.createTempSymbol("debug_rnglist0_start");
    MCOS->emitLabel(RangesSymbol);
    for (MCSection *Sec : Sections) {
      const MCSymbol *StartSymbol = Sec->getBeginSymbol();
      const MCSymbol *EndSymbol = Sec->getEndSymbol(context);
      const MCExpr *SectionStartAddr = MCSymbolRefExpr::create(
          StartSymbol, MCSymbolRefExpr::VK_None, context);
      const MCExpr *SectionSize =
          makeEndMinusStartExpr(context, *StartSymbol, *EndSymbol, 0);
      MCOS->emitInt8(dwarf::DW_RLE_start_length);
      MCOS->emitValue(SectionStartAddr, AddrSize);
      MCOS->emitULEB128Value(SectionSize);
    }
    MCOS->emitInt8(dwarf::DW_RLE_end_of_list);
    MCOS->emitLabel(EndSymbol);
  } else {
    MCOS->SwitchSection(context.getObjectFileInfo()->getDwarfRangesSection());
    RangesSymbol = context.createTempSymbol("debug_ranges_start");
    MCOS->emitLabel(RangesSymbol);
    for (MCSection *Sec : Sections) {
      const MCSymbol *StartSymbol = Sec->getBeginSymbol();
      const MCSymbol *EndSymbol = Sec->getEndSymbol(context);

      // Emit a base address selection entry for the section start.
      const MCExpr *SectionStartAddr = MCSymbolRefExpr::create(
          StartSymbol, MCSymbolRefExpr::VK_None, context);
      MCOS->emitFill(AddrSize, 0xFF);
      MCOS->emitValue(SectionStartAddr, AddrSize);

      // Emit a range list entry spanning this section.
      const MCExpr *SectionSize =
          makeEndMinusStartExpr(context, *StartSymbol, *EndSymbol, 0);
      MCOS->emitIntValue(0, AddrSize);
      emitAbsValue(*MCOS, SectionSize, AddrSize);
    }

    // Emit end of list entry
    MCOS->emitIntValue(0, AddrSize);
    MCOS->emitIntValue(0, AddrSize);
  }

  return RangesSymbol;
}

//
// When generating dwarf for assembly source files this emits the Dwarf
// sections.
//
void MCGenDwarfInfo::Emit(MCStreamer *MCOS) {
  MCContext &context = MCOS->getContext();

  // Create the dwarf sections in this order (.debug_line already created).
  const MCAsmInfo *AsmInfo = context.getAsmInfo();
  bool CreateDwarfSectionSymbols =
      AsmInfo->doesDwarfUseRelocationsAcrossSections();
  MCSymbol *LineSectionSymbol = nullptr;
  if (CreateDwarfSectionSymbols)
    LineSectionSymbol = MCOS->getDwarfLineTableSymbol(0);
  MCSymbol *AbbrevSectionSymbol = nullptr;
  MCSymbol *InfoSectionSymbol = nullptr;
  MCSymbol *RangesSymbol = nullptr;

  // Create end symbols for each section, and remove empty sections
  MCOS->getContext().finalizeDwarfSections(*MCOS);

  // If there are no sections to generate debug info for, we don't need
  // to do anything
  if (MCOS->getContext().getGenDwarfSectionSyms().empty())
    return;

  // We only use the .debug_ranges section if we have multiple code sections,
  // and we are emitting a DWARF version which supports it.
  const bool UseRangesSection =
      MCOS->getContext().getGenDwarfSectionSyms().size() > 1 &&
      MCOS->getContext().getDwarfVersion() >= 3;
  CreateDwarfSectionSymbols |= UseRangesSection;

  MCOS->SwitchSection(context.getObjectFileInfo()->getDwarfInfoSection());
  if (CreateDwarfSectionSymbols) {
    InfoSectionSymbol = context.createTempSymbol();
    MCOS->emitLabel(InfoSectionSymbol);
  }
  MCOS->SwitchSection(context.getObjectFileInfo()->getDwarfAbbrevSection());
  if (CreateDwarfSectionSymbols) {
    AbbrevSectionSymbol = context.createTempSymbol();
    MCOS->emitLabel(AbbrevSectionSymbol);
  }

  MCOS->SwitchSection(context.getObjectFileInfo()->getDwarfARangesSection());

  // Output the data for .debug_aranges section.
  EmitGenDwarfAranges(MCOS, InfoSectionSymbol);

  if (UseRangesSection) {
    RangesSymbol = emitGenDwarfRanges(MCOS);
    assert(RangesSymbol);
  }

  // Output the data for .debug_abbrev section.
  EmitGenDwarfAbbrev(MCOS);

  // Output the data for .debug_info section.
  EmitGenDwarfInfo(MCOS, AbbrevSectionSymbol, LineSectionSymbol, RangesSymbol);
}

//
// When generating dwarf for assembly source files this is called when symbol
// for a label is created.  If this symbol is not a temporary and is in the
// section that dwarf is being generated for, save the needed info to create
// a dwarf label.
//
void MCGenDwarfLabelEntry::Make(MCSymbol *Symbol, MCStreamer *MCOS,
                                     SourceMgr &SrcMgr, SMLoc &Loc) {
  // We won't create dwarf labels for temporary symbols.
  if (Symbol->isTemporary())
    return;
  MCContext &context = MCOS->getContext();
  // We won't create dwarf labels for symbols in sections that we are not
  // generating debug info for.
  if (!context.getGenDwarfSectionSyms().count(MCOS->getCurrentSectionOnly()))
    return;

  // The dwarf label's name does not have the symbol name's leading
  // underbar if any.
  StringRef Name = Symbol->getName();
  if (Name.startswith("_"))
    Name = Name.substr(1, Name.size()-1);

  // Get the dwarf file number to be used for the dwarf label.
  unsigned FileNumber = context.getGenDwarfFileNumber();

  // Finding the line number is the expensive part which is why we just don't
  // pass it in as for some symbols we won't create a dwarf label.
  unsigned CurBuffer = SrcMgr.FindBufferContainingLoc(Loc);
  unsigned LineNumber = SrcMgr.FindLineNumber(Loc, CurBuffer);

  // We create a temporary symbol for use for the AT_high_pc and AT_low_pc
  // values so that they don't have things like an ARM thumb bit from the
  // original symbol. So when used they won't get a low bit set after
  // relocation.
  MCSymbol *Label = context.createTempSymbol();
  MCOS->emitLabel(Label);

  // Create and entry for the info and add it to the other entries.
  MCOS->getContext().addMCGenDwarfLabelEntry(
      MCGenDwarfLabelEntry(Name, FileNumber, LineNumber, Label));
}

static int getDataAlignmentFactor(MCStreamer &streamer) {
  MCContext &context = streamer.getContext();
  const MCAsmInfo *asmInfo = context.getAsmInfo();
  int size = asmInfo->getCalleeSaveStackSlotSize();
  if (asmInfo->isStackGrowthDirectionUp())
    return size;
  else
    return -size;
}

static unsigned getSizeForEncoding(MCStreamer &streamer,
                                   unsigned symbolEncoding) {
  MCContext &context = streamer.getContext();
  unsigned format = symbolEncoding & 0x0f;
  switch (format) {
  default: llvm_unreachable("Unknown Encoding");
  case dwarf::DW_EH_PE_absptr:
  case dwarf::DW_EH_PE_signed:
    return context.getAsmInfo()->getCodePointerSize();
  case dwarf::DW_EH_PE_udata2:
  case dwarf::DW_EH_PE_sdata2:
    return 2;
  case dwarf::DW_EH_PE_udata4:
  case dwarf::DW_EH_PE_sdata4:
    return 4;
  case dwarf::DW_EH_PE_udata8:
  case dwarf::DW_EH_PE_sdata8:
    return 8;
  }
}

static void emitFDESymbol(MCObjectStreamer &streamer, const MCSymbol &symbol,
                       unsigned symbolEncoding, bool isEH) {
  MCContext &context = streamer.getContext();
  const MCAsmInfo *asmInfo = context.getAsmInfo();
  const MCExpr *v = asmInfo->getExprForFDESymbol(&symbol,
                                                 symbolEncoding,
                                                 streamer);
  unsigned size = getSizeForEncoding(streamer, symbolEncoding);
  if (asmInfo->doDwarfFDESymbolsUseAbsDiff() && isEH)
    emitAbsValue(streamer, v, size);
  else
    streamer.emitValue(v, size);
}

static void EmitPersonality(MCStreamer &streamer, const MCSymbol &symbol,
                            unsigned symbolEncoding) {
  MCContext &context = streamer.getContext();
  const MCAsmInfo *asmInfo = context.getAsmInfo();
  const MCExpr *v = asmInfo->getExprForPersonalitySymbol(&symbol,
                                                         symbolEncoding,
                                                         streamer);
  unsigned size = getSizeForEncoding(streamer, symbolEncoding);
  streamer.emitValue(v, size);
}

namespace {

class FrameEmitterImpl {
  int CFAOffset = 0;
  int InitialCFAOffset = 0;
  bool IsEH;
  MCObjectStreamer &Streamer;

public:
  FrameEmitterImpl(bool IsEH, MCObjectStreamer &Streamer)
      : IsEH(IsEH), Streamer(Streamer) {}

  /// Emit the unwind information in a compact way.
  void EmitCompactUnwind(const MCDwarfFrameInfo &frame);

  const MCSymbol &EmitCIE(const MCDwarfFrameInfo &F);
  void EmitFDE(const MCSymbol &cieStart, const MCDwarfFrameInfo &frame,
               bool LastInSection, const MCSymbol &SectionStart);
  void emitCFIInstructions(ArrayRef<MCCFIInstruction> Instrs,
                           MCSymbol *BaseLabel);
  void emitCFIInstruction(const MCCFIInstruction &Instr);
};

} // end anonymous namespace

static void emitEncodingByte(MCObjectStreamer &Streamer, unsigned Encoding) {
  Streamer.emitInt8(Encoding);
}

void FrameEmitterImpl::emitCFIInstruction(const MCCFIInstruction &Instr) {
  int dataAlignmentFactor = getDataAlignmentFactor(Streamer);
  auto *MRI = Streamer.getContext().getRegisterInfo();

  switch (Instr.getOperation()) {
  case MCCFIInstruction::OpRegister: {
    unsigned Reg1 = Instr.getRegister();
    unsigned Reg2 = Instr.getRegister2();
    if (!IsEH) {
      Reg1 = MRI->getDwarfRegNumFromDwarfEHRegNum(Reg1);
      Reg2 = MRI->getDwarfRegNumFromDwarfEHRegNum(Reg2);
    }
    Streamer.emitInt8(dwarf::DW_CFA_register);
    Streamer.emitULEB128IntValue(Reg1);
    Streamer.emitULEB128IntValue(Reg2);
    return;
  }
  case MCCFIInstruction::OpWindowSave:
    Streamer.emitInt8(dwarf::DW_CFA_GNU_window_save);
    return;

  case MCCFIInstruction::OpNegateRAState:
    Streamer.emitInt8(dwarf::DW_CFA_AARCH64_negate_ra_state);
    return;

  case MCCFIInstruction::OpUndefined: {
    unsigned Reg = Instr.getRegister();
    Streamer.emitInt8(dwarf::DW_CFA_undefined);
    Streamer.emitULEB128IntValue(Reg);
    return;
  }
  case MCCFIInstruction::OpAdjustCfaOffset:
  case MCCFIInstruction::OpDefCfaOffset: {
    const bool IsRelative =
      Instr.getOperation() == MCCFIInstruction::OpAdjustCfaOffset;

    Streamer.emitInt8(dwarf::DW_CFA_def_cfa_offset);

    if (IsRelative)
      CFAOffset += Instr.getOffset();
    else
      CFAOffset = Instr.getOffset();

    Streamer.emitULEB128IntValue(CFAOffset);

    return;
  }
  case MCCFIInstruction::OpDefCfa: {
    unsigned Reg = Instr.getRegister();
    if (!IsEH)
      Reg = MRI->getDwarfRegNumFromDwarfEHRegNum(Reg);
    Streamer.emitInt8(dwarf::DW_CFA_def_cfa);
    Streamer.emitULEB128IntValue(Reg);
    CFAOffset = Instr.getOffset();
    Streamer.emitULEB128IntValue(CFAOffset);

    return;
  }
  case MCCFIInstruction::OpDefCfaRegister: {
    unsigned Reg = Instr.getRegister();
    if (!IsEH)
      Reg = MRI->getDwarfRegNumFromDwarfEHRegNum(Reg);
    Streamer.emitInt8(dwarf::DW_CFA_def_cfa_register);
    Streamer.emitULEB128IntValue(Reg);

    return;
  }
  case MCCFIInstruction::OpOffset:
  case MCCFIInstruction::OpRelOffset: {
    const bool IsRelative =
      Instr.getOperation() == MCCFIInstruction::OpRelOffset;

    unsigned Reg = Instr.getRegister();
    if (!IsEH)
      Reg = MRI->getDwarfRegNumFromDwarfEHRegNum(Reg);

    int Offset = Instr.getOffset();
    if (IsRelative)
      Offset -= CFAOffset;
    Offset = Offset / dataAlignmentFactor;

    if (Offset < 0) {
      Streamer.emitInt8(dwarf::DW_CFA_offset_extended_sf);
      Streamer.emitULEB128IntValue(Reg);
      Streamer.emitSLEB128IntValue(Offset);
    } else if (Reg < 64) {
      Streamer.emitInt8(dwarf::DW_CFA_offset + Reg);
      Streamer.emitULEB128IntValue(Offset);
    } else {
      Streamer.emitInt8(dwarf::DW_CFA_offset_extended);
      Streamer.emitULEB128IntValue(Reg);
      Streamer.emitULEB128IntValue(Offset);
    }
    return;
  }
  case MCCFIInstruction::OpRememberState:
    Streamer.emitInt8(dwarf::DW_CFA_remember_state);
    return;
  case MCCFIInstruction::OpRestoreState:
    Streamer.emitInt8(dwarf::DW_CFA_restore_state);
    return;
  case MCCFIInstruction::OpSameValue: {
    unsigned Reg = Instr.getRegister();
    Streamer.emitInt8(dwarf::DW_CFA_same_value);
    Streamer.emitULEB128IntValue(Reg);
    return;
  }
  case MCCFIInstruction::OpRestore: {
    unsigned Reg = Instr.getRegister();
    if (!IsEH)
      Reg = MRI->getDwarfRegNumFromDwarfEHRegNum(Reg);
    if (Reg < 64) {
      Streamer.emitInt8(dwarf::DW_CFA_restore | Reg);
    } else {
      Streamer.emitInt8(dwarf::DW_CFA_restore_extended);
      Streamer.emitULEB128IntValue(Reg);
    }
    return;
  }
  case MCCFIInstruction::OpGnuArgsSize:
    Streamer.emitInt8(dwarf::DW_CFA_GNU_args_size);
    Streamer.emitULEB128IntValue(Instr.getOffset());
    return;

  case MCCFIInstruction::OpEscape:
    Streamer.emitBytes(Instr.getValues());
    return;
  }
  llvm_unreachable("Unhandled case in switch");
}

/// Emit frame instructions to describe the layout of the frame.
void FrameEmitterImpl::emitCFIInstructions(ArrayRef<MCCFIInstruction> Instrs,
                                           MCSymbol *BaseLabel) {
  for (const MCCFIInstruction &Instr : Instrs) {
    MCSymbol *Label = Instr.getLabel();
    // Throw out move if the label is invalid.
    if (Label && !Label->isDefined()) continue; // Not emitted, in dead code.

    // Advance row if new location.
    if (BaseLabel && Label) {
      MCSymbol *ThisSym = Label;
      if (ThisSym != BaseLabel) {
        Streamer.emitDwarfAdvanceFrameAddr(BaseLabel, ThisSym);
        BaseLabel = ThisSym;
      }
    }

    emitCFIInstruction(Instr);
  }
}

/// Emit the unwind information in a compact way.
void FrameEmitterImpl::EmitCompactUnwind(const MCDwarfFrameInfo &Frame) {
  MCContext &Context = Streamer.getContext();
  const MCObjectFileInfo *MOFI = Context.getObjectFileInfo();

  // range-start range-length  compact-unwind-enc personality-func   lsda
  //  _foo       LfooEnd-_foo  0x00000023          0                 0
  //  _bar       LbarEnd-_bar  0x00000025         __gxx_personality  except_tab1
  //
  //   .section __LD,__compact_unwind,regular,debug
  //
  //   # compact unwind for _foo
  //   .quad _foo
  //   .set L1,LfooEnd-_foo
  //   .long L1
  //   .long 0x01010001
  //   .quad 0
  //   .quad 0
  //
  //   # compact unwind for _bar
  //   .quad _bar
  //   .set L2,LbarEnd-_bar
  //   .long L2
  //   .long 0x01020011
  //   .quad __gxx_personality
  //   .quad except_tab1

  uint32_t Encoding = Frame.CompactUnwindEncoding;
  if (!Encoding) return;
  bool DwarfEHFrameOnly = (Encoding == MOFI->getCompactUnwindDwarfEHFrameOnly());

  // The encoding needs to know we have an LSDA.
  if (!DwarfEHFrameOnly && Frame.Lsda)
    Encoding |= 0x40000000;

  // Range Start
  unsigned FDEEncoding = MOFI->getFDEEncoding();
  unsigned Size = getSizeForEncoding(Streamer, FDEEncoding);
  Streamer.emitSymbolValue(Frame.Begin, Size);

  // Range Length
  const MCExpr *Range =
      makeEndMinusStartExpr(Context, *Frame.Begin, *Frame.End, 0);
  emitAbsValue(Streamer, Range, 4);

  // Compact Encoding
  Size = getSizeForEncoding(Streamer, dwarf::DW_EH_PE_udata4);
  Streamer.emitIntValue(Encoding, Size);

  // Personality Function
  Size = getSizeForEncoding(Streamer, dwarf::DW_EH_PE_absptr);
  if (!DwarfEHFrameOnly && Frame.Personality)
    Streamer.emitSymbolValue(Frame.Personality, Size);
  else
    Streamer.emitIntValue(0, Size); // No personality fn

  // LSDA
  Size = getSizeForEncoding(Streamer, Frame.LsdaEncoding);
  if (!DwarfEHFrameOnly && Frame.Lsda)
    Streamer.emitSymbolValue(Frame.Lsda, Size);
  else
    Streamer.emitIntValue(0, Size); // No LSDA
}

static unsigned getCIEVersion(bool IsEH, unsigned DwarfVersion) {
  if (IsEH)
    return 1;
  switch (DwarfVersion) {
  case 2:
    return 1;
  case 3:
    return 3;
  case 4:
  case 5:
    return 4;
  }
  llvm_unreachable("Unknown version");
}

const MCSymbol &FrameEmitterImpl::EmitCIE(const MCDwarfFrameInfo &Frame) {
  MCContext &context = Streamer.getContext();
  const MCRegisterInfo *MRI = context.getRegisterInfo();
  const MCObjectFileInfo *MOFI = context.getObjectFileInfo();

  MCSymbol *sectionStart = context.createTempSymbol();
  Streamer.emitLabel(sectionStart);

  MCSymbol *sectionEnd = context.createTempSymbol();

  dwarf::DwarfFormat Format = IsEH ? dwarf::DWARF32 : context.getDwarfFormat();
  unsigned UnitLengthBytes = dwarf::getUnitLengthFieldByteSize(Format);
  unsigned OffsetSize = dwarf::getDwarfOffsetByteSize(Format);
  bool IsDwarf64 = Format == dwarf::DWARF64;

  if (IsDwarf64)
    // DWARF64 mark
    Streamer.emitInt32(dwarf::DW_LENGTH_DWARF64);

  // Length
  const MCExpr *Length = makeEndMinusStartExpr(context, *sectionStart,
                                               *sectionEnd, UnitLengthBytes);
  emitAbsValue(Streamer, Length, OffsetSize);

  // CIE ID
  uint64_t CIE_ID =
      IsEH ? 0 : (IsDwarf64 ? dwarf::DW64_CIE_ID : dwarf::DW_CIE_ID);
  Streamer.emitIntValue(CIE_ID, OffsetSize);

  // Version
  uint8_t CIEVersion = getCIEVersion(IsEH, context.getDwarfVersion());
  Streamer.emitInt8(CIEVersion);

  if (IsEH) {
    SmallString<8> Augmentation;
    Augmentation += "z";
    if (Frame.Personality)
      Augmentation += "P";
    if (Frame.Lsda)
      Augmentation += "L";
    Augmentation += "R";
    if (Frame.IsSignalFrame)
      Augmentation += "S";
    if (Frame.IsBKeyFrame)
      Augmentation += "B";
    Streamer.emitBytes(Augmentation);
  }
  Streamer.emitInt8(0);

  if (CIEVersion >= 4) {
    // Address Size
    Streamer.emitInt8(context.getAsmInfo()->getCodePointerSize());

    // Segment Descriptor Size
    Streamer.emitInt8(0);
  }

  // Code Alignment Factor
  Streamer.emitULEB128IntValue(context.getAsmInfo()->getMinInstAlignment());

  // Data Alignment Factor
  Streamer.emitSLEB128IntValue(getDataAlignmentFactor(Streamer));

  // Return Address Register
  unsigned RAReg = Frame.RAReg;
  if (RAReg == static_cast<unsigned>(INT_MAX))
    RAReg = MRI->getDwarfRegNum(MRI->getRARegister(), IsEH);

  if (CIEVersion == 1) {
    assert(RAReg <= 255 &&
           "DWARF 2 encodes return_address_register in one byte");
    Streamer.emitInt8(RAReg);
  } else {
    Streamer.emitULEB128IntValue(RAReg);
  }

  // Augmentation Data Length (optional)
  unsigned augmentationLength = 0;
  if (IsEH) {
    if (Frame.Personality) {
      // Personality Encoding
      augmentationLength += 1;
      // Personality
      augmentationLength +=
          getSizeForEncoding(Streamer, Frame.PersonalityEncoding);
    }
    if (Frame.Lsda)
      augmentationLength += 1;
    // Encoding of the FDE pointers
    augmentationLength += 1;

    Streamer.emitULEB128IntValue(augmentationLength);

    // Augmentation Data (optional)
    if (Frame.Personality) {
      // Personality Encoding
      emitEncodingByte(Streamer, Frame.PersonalityEncoding);
      // Personality
      EmitPersonality(Streamer, *Frame.Personality, Frame.PersonalityEncoding);
    }

    if (Frame.Lsda)
      emitEncodingByte(Streamer, Frame.LsdaEncoding);

    // Encoding of the FDE pointers
    emitEncodingByte(Streamer, MOFI->getFDEEncoding());
  }

  // Initial Instructions

  const MCAsmInfo *MAI = context.getAsmInfo();
  if (!Frame.IsSimple) {
    const std::vector<MCCFIInstruction> &Instructions =
        MAI->getInitialFrameState();
    emitCFIInstructions(Instructions, nullptr);
  }

  InitialCFAOffset = CFAOffset;

  // Padding
  Streamer.emitValueToAlignment(IsEH ? 4 : MAI->getCodePointerSize());

  Streamer.emitLabel(sectionEnd);
  return *sectionStart;
}

void FrameEmitterImpl::EmitFDE(const MCSymbol &cieStart,
                               const MCDwarfFrameInfo &frame,
                               bool LastInSection,
                               const MCSymbol &SectionStart) {
  MCContext &context = Streamer.getContext();
  MCSymbol *fdeStart = context.createTempSymbol();
  MCSymbol *fdeEnd = context.createTempSymbol();
  const MCObjectFileInfo *MOFI = context.getObjectFileInfo();

  CFAOffset = InitialCFAOffset;

  dwarf::DwarfFormat Format = IsEH ? dwarf::DWARF32 : context.getDwarfFormat();
  unsigned OffsetSize = dwarf::getDwarfOffsetByteSize(Format);

  if (Format == dwarf::DWARF64)
    // DWARF64 mark
    Streamer.emitInt32(dwarf::DW_LENGTH_DWARF64);

  // Length
  const MCExpr *Length = makeEndMinusStartExpr(context, *fdeStart, *fdeEnd, 0);
  emitAbsValue(Streamer, Length, OffsetSize);

  Streamer.emitLabel(fdeStart);

  // CIE Pointer
  const MCAsmInfo *asmInfo = context.getAsmInfo();
  if (IsEH) {
    const MCExpr *offset =
        makeEndMinusStartExpr(context, cieStart, *fdeStart, 0);
    emitAbsValue(Streamer, offset, OffsetSize);
  } else if (!asmInfo->doesDwarfUseRelocationsAcrossSections()) {
    const MCExpr *offset =
        makeEndMinusStartExpr(context, SectionStart, cieStart, 0);
    emitAbsValue(Streamer, offset, OffsetSize);
  } else {
    Streamer.emitSymbolValue(&cieStart, OffsetSize,
                             asmInfo->needsDwarfSectionOffsetDirective());
  }

  // PC Begin
  unsigned PCEncoding =
      IsEH ? MOFI->getFDEEncoding() : (unsigned)dwarf::DW_EH_PE_absptr;
  unsigned PCSize = getSizeForEncoding(Streamer, PCEncoding);
  emitFDESymbol(Streamer, *frame.Begin, PCEncoding, IsEH);

  // PC Range
  const MCExpr *Range =
      makeEndMinusStartExpr(context, *frame.Begin, *frame.End, 0);
  emitAbsValue(Streamer, Range, PCSize);

  if (IsEH) {
    // Augmentation Data Length
    unsigned augmentationLength = 0;

    if (frame.Lsda)
      augmentationLength += getSizeForEncoding(Streamer, frame.LsdaEncoding);

    Streamer.emitULEB128IntValue(augmentationLength);

    // Augmentation Data
    if (frame.Lsda)
      emitFDESymbol(Streamer, *frame.Lsda, frame.LsdaEncoding, true);
  }

  // Call Frame Instructions
  emitCFIInstructions(frame.Instructions, frame.Begin);

  // Padding
  // The size of a .eh_frame section has to be a multiple of the alignment
  // since a null CIE is interpreted as the end. Old systems overaligned
  // .eh_frame, so we do too and account for it in the last FDE.
  unsigned Align = LastInSection ? asmInfo->getCodePointerSize() : PCSize;
  Streamer.emitValueToAlignment(Align);

  Streamer.emitLabel(fdeEnd);
}

namespace {

struct CIEKey {
  static const CIEKey getEmptyKey() {
    return CIEKey(nullptr, 0, -1, false, false, static_cast<unsigned>(INT_MAX),
                  false);
  }

  static const CIEKey getTombstoneKey() {
    return CIEKey(nullptr, -1, 0, false, false, static_cast<unsigned>(INT_MAX),
                  false);
  }

  CIEKey(const MCSymbol *Personality, unsigned PersonalityEncoding,
         unsigned LSDAEncoding, bool IsSignalFrame, bool IsSimple,
         unsigned RAReg, bool IsBKeyFrame)
      : Personality(Personality), PersonalityEncoding(PersonalityEncoding),
        LsdaEncoding(LSDAEncoding), IsSignalFrame(IsSignalFrame),
        IsSimple(IsSimple), RAReg(RAReg), IsBKeyFrame(IsBKeyFrame) {}

  explicit CIEKey(const MCDwarfFrameInfo &Frame)
      : Personality(Frame.Personality),
        PersonalityEncoding(Frame.PersonalityEncoding),
        LsdaEncoding(Frame.LsdaEncoding), IsSignalFrame(Frame.IsSignalFrame),
        IsSimple(Frame.IsSimple), RAReg(Frame.RAReg),
        IsBKeyFrame(Frame.IsBKeyFrame) {}

  StringRef PersonalityName() const {
    if (!Personality)
      return StringRef();
    return Personality->getName();
  }

  bool operator<(const CIEKey &Other) const {
    return std::make_tuple(PersonalityName(), PersonalityEncoding, LsdaEncoding,
                           IsSignalFrame, IsSimple, RAReg) <
           std::make_tuple(Other.PersonalityName(), Other.PersonalityEncoding,
                           Other.LsdaEncoding, Other.IsSignalFrame,
                           Other.IsSimple, Other.RAReg);
  }

  const MCSymbol *Personality;
  unsigned PersonalityEncoding;
  unsigned LsdaEncoding;
  bool IsSignalFrame;
  bool IsSimple;
  unsigned RAReg;
  bool IsBKeyFrame;
};

} // end anonymous namespace

namespace llvm {

template <> struct DenseMapInfo<CIEKey> {
  static CIEKey getEmptyKey() { return CIEKey::getEmptyKey(); }
  static CIEKey getTombstoneKey() { return CIEKey::getTombstoneKey(); }

  static unsigned getHashValue(const CIEKey &Key) {
    return static_cast<unsigned>(hash_combine(
        Key.Personality, Key.PersonalityEncoding, Key.LsdaEncoding,
        Key.IsSignalFrame, Key.IsSimple, Key.RAReg, Key.IsBKeyFrame));
  }

  static bool isEqual(const CIEKey &LHS, const CIEKey &RHS) {
    return LHS.Personality == RHS.Personality &&
           LHS.PersonalityEncoding == RHS.PersonalityEncoding &&
           LHS.LsdaEncoding == RHS.LsdaEncoding &&
           LHS.IsSignalFrame == RHS.IsSignalFrame &&
           LHS.IsSimple == RHS.IsSimple && LHS.RAReg == RHS.RAReg &&
           LHS.IsBKeyFrame == RHS.IsBKeyFrame;
  }
};

} // end namespace llvm

void MCDwarfFrameEmitter::Emit(MCObjectStreamer &Streamer, MCAsmBackend *MAB,
                               bool IsEH) {
  Streamer.generateCompactUnwindEncodings(MAB);

  MCContext &Context = Streamer.getContext();
  const MCObjectFileInfo *MOFI = Context.getObjectFileInfo();
  const MCAsmInfo *AsmInfo = Context.getAsmInfo();
  FrameEmitterImpl Emitter(IsEH, Streamer);
  ArrayRef<MCDwarfFrameInfo> FrameArray = Streamer.getDwarfFrameInfos();

  // Emit the compact unwind info if available.
  bool NeedsEHFrameSection = !MOFI->getSupportsCompactUnwindWithoutEHFrame();
  if (IsEH && MOFI->getCompactUnwindSection()) {
    bool SectionEmitted = false;
    for (const MCDwarfFrameInfo &Frame : FrameArray) {
      if (Frame.CompactUnwindEncoding == 0) continue;
      if (!SectionEmitted) {
        Streamer.SwitchSection(MOFI->getCompactUnwindSection());
        Streamer.emitValueToAlignment(AsmInfo->getCodePointerSize());
        SectionEmitted = true;
      }
      NeedsEHFrameSection |=
        Frame.CompactUnwindEncoding ==
          MOFI->getCompactUnwindDwarfEHFrameOnly();
      Emitter.EmitCompactUnwind(Frame);
    }
  }

  if (!NeedsEHFrameSection) return;

  MCSection &Section =
      IsEH ? *const_cast<MCObjectFileInfo *>(MOFI)->getEHFrameSection()
           : *MOFI->getDwarfFrameSection();

  Streamer.SwitchSection(&Section);
  MCSymbol *SectionStart = Context.createTempSymbol();
  Streamer.emitLabel(SectionStart);

  DenseMap<CIEKey, const MCSymbol *> CIEStarts;

  const MCSymbol *DummyDebugKey = nullptr;
  bool CanOmitDwarf = MOFI->getOmitDwarfIfHaveCompactUnwind();
  // Sort the FDEs by their corresponding CIE before we emit them.
  // This isn't technically necessary according to the DWARF standard,
  // but the Android libunwindstack rejects eh_frame sections where
  // an FDE refers to a CIE other than the closest previous CIE.
  std::vector<MCDwarfFrameInfo> FrameArrayX(FrameArray.begin(), FrameArray.end());
  llvm::stable_sort(FrameArrayX,
                    [](const MCDwarfFrameInfo &X, const MCDwarfFrameInfo &Y) {
                      return CIEKey(X) < CIEKey(Y);
                    });
  for (auto I = FrameArrayX.begin(), E = FrameArrayX.end(); I != E;) {
    const MCDwarfFrameInfo &Frame = *I;
    ++I;
    if (CanOmitDwarf && Frame.CompactUnwindEncoding !=
          MOFI->getCompactUnwindDwarfEHFrameOnly())
      // Don't generate an EH frame if we don't need one. I.e., it's taken care
      // of by the compact unwind encoding.
      continue;

    CIEKey Key(Frame);
    const MCSymbol *&CIEStart = IsEH ? CIEStarts[Key] : DummyDebugKey;
    if (!CIEStart)
      CIEStart = &Emitter.EmitCIE(Frame);

    Emitter.EmitFDE(*CIEStart, Frame, I == E, *SectionStart);
  }
}

void MCDwarfFrameEmitter::EmitAdvanceLoc(MCObjectStreamer &Streamer,
                                         uint64_t AddrDelta) {
  MCContext &Context = Streamer.getContext();
  SmallString<256> Tmp;
  raw_svector_ostream OS(Tmp);
  MCDwarfFrameEmitter::EncodeAdvanceLoc(Context, AddrDelta, OS);
  Streamer.emitBytes(OS.str());
}

void MCDwarfFrameEmitter::EncodeAdvanceLoc(MCContext &Context,
                                           uint64_t AddrDelta, raw_ostream &OS,
                                           uint32_t *Offset, uint32_t *Size) {
  // Scale the address delta by the minimum instruction length.
  AddrDelta = ScaleAddrDelta(Context, AddrDelta);

  bool WithFixups = false;
  if (Offset && Size)
    WithFixups = true;

  support::endianness E =
      Context.getAsmInfo()->isLittleEndian() ? support::little : support::big;
  if (AddrDelta == 0) {
    if (WithFixups) {
      *Offset = 0;
      *Size = 0;
    }
  } else if (isUIntN(6, AddrDelta)) {
    uint8_t Opcode = dwarf::DW_CFA_advance_loc | AddrDelta;
    if (WithFixups) {
      *Offset = OS.tell();
      *Size = 6;
      OS << uint8_t(dwarf::DW_CFA_advance_loc);
    } else
      OS << Opcode;
  } else if (isUInt<8>(AddrDelta)) {
    OS << uint8_t(dwarf::DW_CFA_advance_loc1);
    if (WithFixups) {
      *Offset = OS.tell();
      *Size = 8;
      OS.write_zeros(1);
    } else
      OS << uint8_t(AddrDelta);
  } else if (isUInt<16>(AddrDelta)) {
    OS << uint8_t(dwarf::DW_CFA_advance_loc2);
    if (WithFixups) {
      *Offset = OS.tell();
      *Size = 16;
      OS.write_zeros(2);
    } else
      support::endian::write<uint16_t>(OS, AddrDelta, E);
  } else {
    assert(isUInt<32>(AddrDelta));
    OS << uint8_t(dwarf::DW_CFA_advance_loc4);
    if (WithFixups) {
      *Offset = OS.tell();
      *Size = 32;
      OS.write_zeros(4);
    } else
      support::endian::write<uint32_t>(OS, AddrDelta, E);
  }
}
