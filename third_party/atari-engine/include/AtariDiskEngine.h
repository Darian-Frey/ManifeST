/**
 * @file AtariDiskEngine.h
 * @brief Core classes and utilities for handling Atari ST disk images.
 */

#ifndef ATARIDISKENGINE_H
#define ATARIDISKENGINE_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @namespace Atari
 * @brief Contains all classes and functions related to Atari ST disk
 * operations.
 */
namespace Atari {

// Forward-declared so AtariDiskEngine can `friend` it. Defined in
// AtariDiskEngine.cpp — RAII helper used to mark composite repair
// operations so nested pushUndoSnapshot() calls coalesce.
struct CompositeOpGuard;

/** @brief Standard sector size for Atari ST disks (512 bytes). */
inline constexpr uint16_t SECTOR_SIZE = 512;
/** @brief Size of a directory entry in FAT12 (32 bytes). */
inline constexpr uint16_t DIRENT_SIZE = 32;
/** @brief Target value for the boot sector checksum (0x1234). */
inline constexpr uint16_t BOOT_CHECKSUM_TARGET = 0x1234;

// ── FAT12 entry sentinel values ─────────────────────────────────────────────
// Spec: each entry is 12 bits.
//   0x000             — free cluster
//   0x002 .. 0xFEF    — valid data clusters / next-cluster pointers
//   0xFF0 .. 0xFF6    — reserved (rarely used)
//   0xFF7             — bad cluster
//   0xFF8 .. 0xFFF    — end-of-chain (any value in this range means EOC)
//   0xFFF             — canonical value the engine writes when terminating a chain

/** @brief FAT12 free-cluster marker. */
inline constexpr uint16_t FAT12_FREE         = 0x000;
/** @brief Smallest reserved cluster value. Cluster numbers >= this are not
 *  valid data clusters and indicate either bad-cluster or end-of-chain. */
inline constexpr uint16_t FAT12_RESERVED_MIN = 0xFF0;
/** @brief Bad-cluster marker. */
inline constexpr uint16_t FAT12_BAD          = 0xFF7;
/** @brief Smallest end-of-chain value. Any FAT12 entry with a value in
 *  [FAT12_EOC_MIN, FAT12_EOC] terminates a cluster chain. */
inline constexpr uint16_t FAT12_EOC_MIN      = 0xFF8;
/** @brief Canonical end-of-chain value the engine writes to terminate chains
 *  and returns from FAT-read helpers on out-of-bounds / invalid input. */
inline constexpr uint16_t FAT12_EOC          = 0xFFF;

/**
 * @brief Reads a 16-bit little-endian value from a buffer.
 * @param p Pointer to the start of the 16-bit value.
 * @return The 16-bit value in host byte order.
 */
inline uint16_t readLE16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/**
 * @brief Reads a 16-bit big-endian value from a buffer.
 * @param p Pointer to the start of the 16-bit value.
 * @return The 16-bit value in host byte order.
 */
inline uint16_t readBE16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

/**
 * @brief Writes a 16-bit value in little-endian format to a buffer.
 * @param p Pointer to the destination buffer.
 * @param val The 16-bit value to write.
 */
inline void writeLE16(uint8_t *p, uint16_t val) {
  p[0] = val & 0xFF;
  p[1] = (val >> 8) & 0xFF;
}

/**
 * @brief Writes a 16-bit value in big-endian format to a buffer.
 * @param p Pointer to the destination buffer.
 * @param val The 16-bit value to write.
 */
inline void writeBE16(uint8_t *p, uint16_t val) {
  p[0] = (val >> 8) & 0xFF;
  p[1] = val & 0xFF;
}

/**
 * @brief Decodes a single FAT12 entry from a buffer, with bounds checking.
 *
 * FAT12 packs each 12-bit entry into 1.5 bytes. For an even cluster N, the
 * entry is bits 0-11 of the 16-bit word at byte offset (N*3)/2. For an odd
 * cluster, the entry is bits 4-15 of the same word.
 *
 * @param fat     Pointer to the start of the FAT region.
 * @param fatLen  Number of bytes available from `fat` onwards.
 * @param cluster Cluster number to read.
 * @return The 12-bit FAT entry, or `FAT12_EOC` if `cluster` is negative,
 *         exceeds the FAT12 maximum, or the read would go out of bounds.
 *         Callers treat values >= `FAT12_EOC_MIN` as end-of-chain, so an OOB
 *         read terminates walks safely instead of crashing.
 */
inline uint16_t readFAT12(const uint8_t *fat, size_t fatLen, int cluster) {
  if (cluster < 0 || cluster > FAT12_EOC)
    return FAT12_EOC;
  size_t idx = (static_cast<size_t>(cluster) * 3) / 2;
  if (idx + 1 >= fatLen)
    return FAT12_EOC;
  uint16_t raw = static_cast<uint16_t>(fat[idx] | (fat[idx + 1] << 8));
  return (cluster & 1) ? (raw >> 4) : (raw & 0x0FFF);
}

/**
 * @brief Writes a single FAT12 entry to a buffer, with bounds checking.
 *
 * @param fat     Pointer to the start of the FAT region.
 * @param fatLen  Number of bytes available from `fat` onwards.
 * @param cluster Cluster number to write.
 * @param value   12-bit value to store. Bits 12-15 are ignored.
 * @return true on success, false if the cluster is out of range or the write
 *         would overflow `fatLen`.
 */
inline bool writeFAT12(uint8_t *fat, size_t fatLen, int cluster,
                       uint16_t value) {
  if (cluster < 0 || cluster > FAT12_EOC)
    return false;
  size_t idx = (static_cast<size_t>(cluster) * 3) / 2;
  if (idx + 1 >= fatLen)
    return false;
  if (cluster & 1) {
    fat[idx]     = static_cast<uint8_t>((fat[idx] & 0x0F) | ((value << 4) & 0xF0));
    fat[idx + 1] = static_cast<uint8_t>((value >> 4) & 0xFF);
  } else {
    fat[idx]     = static_cast<uint8_t>(value & 0xFF);
    fat[idx + 1] = static_cast<uint8_t>((fat[idx + 1] & 0xF0) | ((value >> 8) & 0x0F));
  }
  return true;
}

/**
 * @struct DiskStats
 * @brief Contains statistics about the disk image.
 */
struct DiskStats {
  QString label;
  uint64_t totalBytes;
  uint64_t freeBytes;
  uint64_t usedBytes;
  int fileCount = 0;
  int dirCount = 0;
  int sectorsPerCluster = 2; // Default to 2
  int totalClusters = 0;
  int freeClusters = 0;
  
  // Added for geometry overrides
  int sectorsPerTrack = 9;
  int sides = 2;
  int totalSectors = 1440;
  int dataStartSector = 18;
  int reservedSectors = 1;
  int numFATs = 2;
  int fatSizeSectors = 0;
  int rootDirectoryEntries = 0;
};

/**
 * @struct DirEntry
 * @brief Represents a single directory entry in the FAT12 filesystem.
 */
struct DirEntry {
  uint8_t name[8];         /**< Filename (space padded). */
  uint8_t ext[3];          /**< Extension (space padded). */
  uint8_t attr;            /**< File attributes. */
  uint8_t reserved[10];    /**< Reserved area. */
  uint8_t time[2];         /**< Modification time. */
  uint8_t date[2];         /**< Modification date. */
  uint8_t startCluster[2]; /**< Starting cluster of the file. */
  uint8_t fileSize[4];     /**< File size in bytes. */

  /** @return True if this entry represents a directory. */
  bool isDirectory() const { return attr & 0x10; }

  /** @return The starting cluster number (little-endian conversion). */
  uint16_t getStartCluster() const { return readLE16(startCluster); }

  /** @return The file size in bytes (little-endian conversion). */
  uint32_t getFileSize() const {
    return fileSize[0] | (fileSize[1] << 8) | (fileSize[2] << 16) |
           (fileSize[3] << 24);
  }

  /** @return The reconstructed filename string "NAME.EXT". */
  std::string getFilename() const;

  /** @return Filename with bit 7 stripped from each byte (D-Bug high-bit decode). */
  std::string getFilenameHighBitStripped() const;
};

struct BootSectorInfo {
  bool hasValidBpb;
  bool isExecutable;
  uint16_t currentChecksum;
  uint16_t expectedChecksum; // 0x1234
  QString oemName;
};

struct BootSectorBpb {
  QString oemName;
  uint16_t bytesPerSector;
  uint8_t sectorsPerCluster;
  uint16_t reservedSectors;
  uint8_t fatCount;
  uint16_t rootEntries;
  uint16_t totalSectors;
  uint8_t mediaDescriptor;
  uint16_t sectorsPerFat;
  uint16_t sectorsPerTrack;
  uint16_t sides;
  uint16_t hiddenSectors;
};

enum class ClusterStatus {
  Free,
  Used,
  Fragmented,
  CrossLinked,
  Bad,
  EndOfChain,
  System,
  Orphaned
};

struct ClusterMap {
  QVector<ClusterStatus> clusters;
  int totalClusters;
};

struct DiskHealthReport {
    bool hasGeometryIssue = false;
    bool hasFatMismatch = false;
    bool isOptimizedDisk = false;
    int orphanedClusterCount = 0;
    int crossLinkedClusterCount = 0;
    std::vector<uint32_t> orphanedClusters;
    std::vector<uint32_t> crossLinkedClusters;
    std::vector<QString> invalidDirEntries;
    std::vector<uint32_t> invalidDirEntryOffsets;
    std::vector<uint16_t> brokenChainEnds;
    std::vector<QString> warnings;
    
    int totalClusters = 0;
    int freeClusters = 0;
    int usedClusters = 0;
    int badClusters = 0;
};

struct SearchResult {
  uint32_t offset;
  int sector;
  int offsetInSector;
};

/**
 * @class AtariDiskEngine
 * @brief Engine for reading, writing, and manipulating Atari ST floppy disk
 * images.
 *
 * This class handles FAT12 filesystem structures, boot sector validation,
 * and high-level file operations within a disk image.
 */
class AtariDiskEngine {
public:
  /**
   * @brief Gets statistics about the disk image.
   * @return DiskStats struct with disk information.
   */
  DiskStats getDiskStats() const;

  /**
   * @brief Deletes a file from the disk image.
   * @param entry The directory entry of the file to delete.
   * @param parentCluster The cluster of the parent directory (0 for root).
   * @return True if successful.
   */
  bool deleteFile(const DirEntry &entry, uint16_t parentCluster = 0);
  
  /**
   * @brief Deletes a directory and all of its contents recursively.
   * @param dirEntry The directory entry of the directory to delete.
   * @param parentCluster The cluster of the parent directory (0 for root).
   * @return True if successful.
   */
  bool deleteDirectory(const DirEntry &dirEntry, uint16_t parentCluster = 0);

  /** @brief Modes for interpreting disk geometry. */
  enum class GeometryMode { Unknown, BPB, HatariGuess };

  /** @brief Known cracking/demo groups, detected from boot sector signatures.
   *  Used both for UI labelling and to drive read-path quirks (currently
   *  only D-Bug has read-path special-cases). The "raw boot loader" groups
   *  below are detection-only — the engine has nothing to extract from
   *  their disks beyond the boot text itself, since these disks are
   *  single-game cracks with no FAT12 filesystem at all. */
  enum class Group {
    Unknown,
    DBug,
    PompeyPirates,
    MedwayBoys,
    Cynix,         ///< Cynix Loader (Duckula et al.)
    Elite,         ///< Elite / Teknix boot-sector cracks
    TRSI,          ///< Tristar/Red Sector
    Metallinos,    ///< Metallinos boot loader
    CSM,           ///< Cracking Service Munich
    Replicants,    ///< Replicants
    NeuterBooter,  ///< "Neuter Booter" generic loader
    Copylock,      ///< Rob Northen Copylock-protected original
    Sleepwalker,   ///< Sleepwalker multi-disk data part
    MsDosFloppy    ///< MS-DOS / PC Tools formatted floppy (not an Atari disk)
  };

  /** @return The detected cracker/demo group for this disk. */
  Group getGroup() const { return m_group; }

  /** @return Human-readable name for the detected group, or empty string. */
  QString getGroupName() const;

  /** @return True if filenames are suspected to use D-Bug high-bit encoding. */
  bool isHighBitEncoded() const { return m_highBitEncoded; }

  /** @return True if the loaded disk appears to be a raw boot-sector
   *  loader (single-game crack) rather than a FAT12 volume. Set when
   *  bpbIsSane() and bruteForceGeometry() both fail and a known cracker
   *  signature was detected in the boot sector, OR when the BPB-walked
   *  root directory yields zero entries on a disk with a recognised
   *  group signature. The UI should show the boot strings instead of
   *  pretending the file tree is broken. */
  bool isRawLoaderDisk() const { return m_isRawLoaderDisk; }

  /** @return True if the dump appears incomplete (image < 700 KB on
   *  what should be an Atari ST floppy, or all-zero boot sector). */
  bool isShortDump() const { return m_isShortDump; }

  /** @return Printable ASCII strings extracted from the boot sector
   *  (sector 0). Returns runs of >= 6 printable characters. Used by the
   *  UI to surface custom-loader information when there's no filesystem
   *  to display. */
  QStringList getBootSectorStrings() const;

  /**
   * @brief Returns the best decoded display name for a directory entry.
   * If the disk is high-bit encoded (D-Bug), strips bit 7 from name bytes.
   * If still unreadable, returns an empty string (caller should show fallback).
   */
  QString getDecodedName(const DirEntry &e) const;

  /**
   * @brief Scans sectors 1\u201314 for plausible 32-byte directory entries.
   * Used as a rescue fallback for D-Bug disks whose FAT chain is scrambled.
   */
  std::vector<DirEntry> deepCarveDirEntries() const;

  /**
   * @brief Injects a local file into the disk image.
   * @param localPath Path to the file on the host system.
   * @param targetCluster Cluster of target folder (0 for root).
   * @return True if successful.
   */
  bool injectFile(const QString &localPath, uint16_t targetCluster = 0);

  /**
   * @brief Reads a file content from the disk image as a QByteArray.
   * @param entry The directory entry of the file to read.
   * @return File contents.
   */
  QByteArray readFileQt(const DirEntry &entry) const;

  /** @brief Checks the boot sector for validity and executable status. */
  BootSectorInfo checkBootSector() const;

  /** @return Const reference to the raw disk image data. */
  const std::vector<uint8_t> &getRawImageData() const { return m_image; }

  /** @brief Snapshot the current image onto the undo stack. Called by
   *  every modification entry point (inject, delete, rename, format,
   *  repair) immediately before mutating m_image. The stack is capped
   *  at kUndoStackLimit; oldest snapshots are dropped when the cap is
   *  reached. Pushing a new snapshot also clears the redo stack —
   *  conventional editor semantics. */
  void pushUndoSnapshot();

  /** @brief Pop the most recent undo snapshot and restore m_image.
   *  Pushes the previous m_image onto the redo stack so the operation
   *  can be re-applied with redo(). Returns false if there's nothing
   *  to undo. The caller is responsible for refreshing any cached
   *  state (e.g. the file tree model) after a successful undo. */
  bool undo();

  /** @brief Re-apply the most recently undone modification. Returns
   *  false if there's nothing to redo. */
  bool redo();

  /** @return True if undo() would succeed. */
  bool canUndo() const { return !m_undoStack.empty(); }

  /** @return True if redo() would succeed. */
  bool canRedo() const { return !m_redoStack.empty(); }

  /** @brief Fixes the boot sector checksum to make the disk executable. */
  bool fixBootChecksum();

  /** @brief Validates checksum for an arbitrary 512-byte buffer. */
  AtariDiskEngine() = default;

  /** @brief Constructs an engine with existing image data. */
  AtariDiskEngine(std::vector<uint8_t> imageData);

  /** @brief Constructs an engine from a raw data buffer. */
  AtariDiskEngine(const uint8_t *data, std::size_t byteCount);

  /** @brief Loads disk image data into the engine. */
  void load(const std::vector<uint8_t> &data);

  /** @return True if an image is currently loaded. */
  bool isLoaded() const { return !m_image.empty(); }

  /** @return True if the loaded image is read-only (e.g. an STX dump where
   *  re-encoding would lose copy protection metadata). The UI should grey
   *  out inject / delete / rename / format / repair when this is true. */
  bool isReadOnly() const { return m_isReadOnly; }

  /** @return List of entries in the root directory. */
  std::vector<DirEntry> readRootDirectory(std::vector<uint32_t>* offsets = nullptr) const;

  /** @return List of entries in a subdirectory starting at the given cluster.
   */
  std::vector<DirEntry> readSubDirectory(uint16_t startCluster, std::vector<uint32_t>* offsets = nullptr) const;

  /** @return Raw bytes of a file specified by its directory entry. */
  std::vector<uint8_t> readFile(const DirEntry &entry) const;

  /** @brief Loads an image from a file path. */
  bool loadImage(const QString &path);

  /** @brief Saves the current image to a file path. */
  bool saveImage(const QString &path);

  /** @return The path of the currently loaded image. */
  QString getFilePath() const { return m_currentFilePath; }

  /** @return Raw data of a specific sector. */
  QByteArray getSector(uint32_t sectorIndex) const;

  /** @return The geometry mode used for the current image. */
  GeometryMode getGeometryMode() const { return m_geoMode; }

  /** @return True if engine has fallen back to manual override mode. */
  bool isUsingManualOverride() const { return m_useManualOverride; }

  /** @return A human-readable string describing the disk format. */
  QString getFormatInfoString() const;

  /** @brief Utility to convert std::string to QString. */
  static QString toQString(const std::string &s);

  /** @return True if the boot sector checksum (executable flag) is valid. */
  bool validateBootChecksum() const noexcept;

  /** @brief Validates checksum for an arbitrary 512-byte buffer. */
  static bool validateBootChecksum(const uint8_t *sector512) noexcept;

  /**
   * @brief Creates a new blank disk image of a specific geometry.
   * @param tracks Number of tracks.
   * @param sectorsPerTrack Sectors per track.
   * @param sides Number of sides (1 or 2).
   */
  void createBlankDisk(int tracks, int sectorsPerTrack, int sides);

  /** @brief Displays raw file info or similar debug tools. */
  void debugPrintDirectory(const std::vector<DirEntry> &dir) const;

  /**
   * @brief Analyzes the FAT array and returns a fully classified map of all clusters.
   * Helps identify free, used, fragmented, cross-linked, and bad sectors.
   */
  ClusterMap analyzeFat() const;

  /** @brief Renames a file in the disk image.
   *  @param parentCluster Cluster of the parent directory (0 = root). Required
   *  to support renaming files inside subdirectories. */
  bool renameFile(const DirEntry &entry, const QString &newName,
                  uint16_t parentCluster = 0);

  /** @brief Gets the file data for a specific directory entry. */
  QByteArray getFileData(const DirEntry &entry) const;

  /** @brief Formats the disk image with a standard 720KB empty format. */
  bool formatDisk();

  /** @brief Reads the entire BPB configuration from Sector 0. */
  BootSectorBpb getBpb() const;

  /** @brief Writes the BPB configuration back to Sector 0 and fixes the checksum. */
  bool setBpb(const BootSectorBpb &bpb);

  /** @return The 12-bit value of a specific FAT cluster entry. */
  uint16_t readFATEntry(int cluster) const;

  /** @brief Sets the 12-bit value of a specific FAT cluster entry. */
  void setFATEntry(int cluster, uint16_t value);

  /** @return True if FAT1 and FAT2 are identical. */
  bool isFatSymmetric() const;

  /** @brief Synchronizes FAT2 to exactly match FAT1. */
  bool syncFat1ToFat2();

  /** @return A list of clusters marked as used in FAT but not referenced by any file. */
  std::vector<uint16_t> findOrphanedClusters() const;

  /** @brief Creates RECOVxxx.REC entries for orphaned cluster chains. */
  bool adoptOrphanClusters();

  /** @brief Finds the slack space at the end of a file's cluster chain and zero-fills it. */
  bool wipeSlackSpace(const DirEntry &entry);

  /** @brief Gets a map of all clusters on the disk. */
  ClusterMap getClusterMap() const;
  
  /** @brief Performs a full disk integrity audit. */
  DiskHealthReport auditDisk() const;

  /** @brief Automatically repairs fixable errors found in the audit report. */
  bool repairDiskHealth();

  struct HighlightingRange {
      uint32_t start;
      uint32_t end;
      enum class Type {
          Boot,
          FAT1,
          FAT2,
          Root,           ///< Normal root directory
          RootScrambled,  ///< Root has garbage names — highlight red
          RootRecovered,  ///< Root was recovered by Linear Hunt — highlight green
          File,
          BpbDiskGeometry,  ///< BPB fields: bytes/sector, total sectors, SPT, sides
          BpbFatLayout,     ///< BPB fields: reserved, FAT count, FAT size, media
          BpbDirLayout      ///< BPB fields: root entries, sectors/cluster
      } type;
      QString label;
  };

  /** @brief Result of probing a specific sector as a potential root. */
  struct ScrambleProbe {
      bool found = false;       ///< True if a clean sector was located
      uint32_t sector = 0;      ///< Sector number of the clean root
      int cleanCount  = 0;      ///< How many valid entries were found
  };

  /** @return True if the last readRootDirectory() found scrambled/garbage names. */
  bool isScrambled() const { return m_isScrambled; }

  /** @return A human-readable status string describing scramble state. */
  QString getScrambleStatusString() const;

  /**
   * @brief Scans sectors 1–20 to find the first sector that yields clean names.
   *  Locks m_manualRootSector and refreshes if successful.
   *  @return True if a valid root was found and locked in.
   */
  bool tryLinearRootRecovery();

  /** @return Layout metadata for coloring the hex viewer. */
  std::vector<HighlightingRange> getDiskLayoutMetadata() const;

  /** @return Byte ranges for a specific file's cluster chain. */
  std::vector<HighlightingRange> getFileHighlighting(const DirEntry &entry) const;

  struct FileSignature {
      QString name;
      QString category;
      QByteArray pattern;
      QString description;
  };

  /** @return A list of predefined signatures for Atari ST and common formats. */
  static std::vector<FileSignature> getWellKnownSignatures();

  /** @brief Searches for a byte pattern in the disk image. */
  QVector<SearchResult> searchPattern(const QByteArray &pattern) const;

  /** @brief Creates a new sub-directory in the root directory. */
  bool createDirectory(const QString &dirName);

  /** @return Byte offset to the first FAT. */
  uint32_t fat1Offset() const noexcept;

  /** @return Byte offset to the start of a specific cluster (Hatari style). */
  uint32_t clusterToByteOffset(uint32_t cluster) const noexcept;

  /** @brief Guesses geometry from file size and updates BPB. */
  bool repairGeometry();

  /** @brief Recalculates FAT sectors based on image size and clusters. */
  bool repairFatSize();

  /** @brief Aligns BPB fields (reservedSectors) to match the root directory's actual location. */
  bool repairRootOffset();

  /** @brief Scans first 50 sectors to locate real root directory offset. */
  uint32_t huntForRootDirectory();

private:
  /** @brief Scans for the root directory if BPB is absent or invalid */
  bool bruteForceGeometry();

  /** @brief Checks if a block of data appears to be a valid directory entry. */
  bool isValidDirectoryEntry(const uint8_t *data) const;
  
  /** @brief Overrides geometry config if matching specific sizes. */
  void validateGeometryBySize();

  /** @brief Decodes an MSA format file into standard RAW sector format in memory. */
  bool decodeMSA();

  /** @brief Encodes the raw image as MSA format data. Returns empty vector on failure. */
  std::vector<uint8_t> encodeMSA() const;

  /** @brief Decodes a Pasti STX format file into a flat sector image in memory.
   *
   * STX preserves track-level metadata for copy-protected disks. We
   * extract the canonical sector data only and discard protection metadata
   * (sector timing, weak/fuzzy bits, multiple sector passes), so the
   * resulting image is read-only — re-encoding to STX would lose the
   * protection. The engine sets m_isReadOnly = true after a successful
   * STX load.
   *
   * @return true if the file was a valid STX and was decoded successfully.
   */
  bool decodeSTX();

  /** @brief Internal initialization after data load. */
  void init();

  /** @return The next cluster in the chain from the FAT. */
  uint16_t getNextCluster(uint16_t currentCluster) const noexcept;

  /** @return List of all cluster indices in a file's chain. */
  std::vector<uint16_t> getClusterChain(uint16_t startCluster) const;

  std::vector<uint8_t> m_image;
  uint32_t m_internalOffset = 0;
  mutable GeometryMode m_geoMode = GeometryMode::Unknown;
  QString m_currentFilePath;
  uint32_t m_manualRootSector = 11;
  bool m_useManualOverride = false;
  bool m_isReadOnly = false;  ///< true after loading an STX (no inject/format/repair)
  mutable bool m_isScrambled = false;
  mutable int  m_scrambledGarbageCount = 0;
  mutable int  m_scrambledTotalEntries = 0;
  Group m_group              = Group::Unknown;
  bool  m_highBitEncoded     = false;
  bool  m_isRawLoaderDisk    = false;
  bool  m_isShortDump        = false;

  /** @brief Undo / redo snapshot stacks. Each entry is a full copy of
   *  m_image taken at the moment a modification was about to happen.
   *  Capped at kUndoStackLimit (10 levels) to bound memory; on a 1 MB
   *  floppy that's ~10 MB worst case. Snapshots are taken from the
   *  outermost modification entry points (inject, delete, rename,
   *  format, repair) — see pushUndoSnapshot(). */
  static constexpr size_t kUndoStackLimit = 10;
  std::vector<std::vector<uint8_t>> m_undoStack;
  std::vector<std::vector<uint8_t>> m_redoStack;

  /** @brief Set while a composite repair (e.g. repairDiskHealth) is
   *  running. Causes pushUndoSnapshot() to skip — the umbrella took
   *  one snapshot for the whole operation, and individual sub-repair
   *  calls within it should NOT each take their own snapshots
   *  (otherwise undoing "Repair All" would only roll back the last
   *  sub-step). Manipulated via the CompositeOpGuard RAII helper. */
  bool m_inCompositeOp = false;
  friend struct CompositeOpGuard;

  DiskStats m_stats;

  /** @brief Port of the test-tool whitelist check — returns true for valid TOS chars. */
  static bool isAtariLegalChar(uint8_t c);
  /** @brief Returns true if a filename looks like garbage/random characters. */
  static bool isNameGarbage(const std::string &name);
  /** @brief Returns true if > 25% of valid entries in a directory are garbage. */
  static bool isDirectoryGarbage(const std::vector<DirEntry> &entries,
                                  int &garbageCount, int &totalCount);
  /** @brief Returns a count of plausible directory entries in the given sector.
   *  Used by huntForRootDirectory() when BPB geometry is clearly invalid. */
  int scoreAsRootDirectory(uint32_t sector) const;

  void writeLE32(uint8_t *ptr, uint32_t val);
  int findFreeCluster() const;
  void freeClusterChain(uint16_t startCluster);

  /** @brief Returns the absolute byte offsets of every 32-byte directory
   *  slot in the directory at parentCluster. parentCluster=0 walks the
   *  fixed-size root directory; non-zero clusters walk the full FAT chain
   *  of a subdirectory (so directories larger than one cluster are
   *  scanned correctly). */
  std::vector<uint32_t> collectDirectorySlots(uint16_t parentCluster) const;

  /** @brief Validates the BPB at offset 0 against a tighter set of bounds
   *  than the existing checkBootSector().hasValidBpb test. Used by init()
   *  to reject obviously-trashed boot sectors (e.g. nulled, x86 DOS, raw
   *  cracker loader hijack) before they contaminate downstream geometry.
   *  Returns false on any out-of-range field; the caller should then fall
   *  through to bruteForceGeometry(). */
  bool bpbIsSane() const;

  /** @brief Result of a single directory-tree walk over the loaded disk.
   *  Populated by walkDirTree(); consumed by auditDisk(), analyzeFat(),
   *  and findOrphanedClusters() instead of each one re-implementing the
   *  walk with its own subtly different cycle guards and validators.
   *
   *  refCount[c] = number of directory entries whose cluster chain
   *  includes cluster c. Cluster 0 / 1 are always 0.
   *  brokenChainEnds = clusters holding a structurally invalid next
   *  pointer (next == 0, self-link, or chain length exceeded total).
   *  invalidDirEntryNames / invalidDirEntryOffsets describe directory
   *  entries that point at impossible start clusters. */
  struct DirTreeWalkResult {
    int totalClusters = 0;
    std::vector<int> refCount;
    std::vector<uint16_t> brokenChainEnds;
    std::vector<QString> invalidDirEntryNames;
    std::vector<uint32_t> invalidDirEntryOffsets;
  };

  /** @brief Walks the entire directory tree starting at the root,
   *  recursing into subdirectories with cycle protection, and produces
   *  a DirTreeWalkResult. Single source of truth for cluster-reachability
   *  analysis used by audit / repair / FAT visualisation. Replaces three
   *  copy-pasted walk implementations that previously each had their own
   *  subtly different cycle guards. */
  DirTreeWalkResult walkDirTree() const;
};

} // namespace Atari
#endif
