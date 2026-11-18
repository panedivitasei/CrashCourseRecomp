// The game's leaderboard page (leaderboards.h): a worker thread caches the server's copy of speedrun.com's verified
// level times, and the game's stats enumerations and friends reads are answered from it on the game's own thread.

#include "leaderboards.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xio.h>
#include <rex/types.h>

#include "../online.h"

// The runtime's implementations (exported from the runtime DLL).
namespace rex::kernel::xam {
u32 XamEnumerate_entry(u32 handle, u32 flags, mapped_void buffer, u32 buffer_length, mapped_u32 items_returned,
                       ppc_ptr_t<rex::system::XAM_OVERLAPPED> overlapped);
u32 XMsgCancelIORequest_entry(ppc_ptr_t<rex::system::XAM_OVERLAPPED> overlapped_ptr, u32 wait);
}  // namespace rex::kernel::xam

REX_EXTERN(__imp__sub_821B09B8);  // swprintf(out, 510, format, ...)
REX_EXTERN(__imp__sub_8297EEC0);  // NETMANXA: the page's "filter - type" label (netman, course, out)
REX_EXTERN(__imp__sub_829801D8);  // NETMANXA::LoadLeaderboard(netman, course, filter)
REX_EXTERN(__imp__sub_829843F0);  // NETMANXA::NextLeaderboardFilter(result, netman)
REX_EXTERN(__imp__sub_829844A0);  // NETMANXA::PrevLeaderboard(result, netman)
REX_EXTERN(__imp__sub_82984590);  // NETMANXA::NextLeaderboard(result, netman)
REX_EXTERN(__imp__sub_82984698);  // NETMANXA::NextLeaderboardType(result, netman)

namespace cc1::boards {
namespace {

using rex::u32;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kIoPending = 997;
constexpr uint32_t kRowSize = 0x30, kColumnSize = 0x18, kHeaderSize = 0x18;  // a row; a column; results + one view
constexpr uint32_t kFriendRows = 50;  // what the friends view takes
constexpr uint32_t kTagOffset = 0x18, kTagSize = 15;  // a row's gamertag field, less its terminator
constexpr uint32_t kXuidHigh = 0xE0000000u, kXuidBase = 0x00100000u, kXuidStep = 0x01000000u;
constexpr uint32_t kNameFormat = 0x8214C5AC;  // L"%S", what the game copies a row's gamertag with
constexpr uint32_t kNameRoom = 509;           // characters that swprintf may write
// NETMANXA: the leaderboard commands are off, the filter, the small panels' centered flag, the course, the view
// of the last load.
constexpr uint32_t kOff = 0x28064, kFilter = 0x280A8, kCentered = 0x280B1, kCourse = 0x280E4, kLastView = 0x280E8;
constexpr uint32_t kOverall = 0, kFriends = 1, kMyScore = 2;
// A board's versions: the server's path for each, and the page's name for it.
constexpr uint32_t kVersions = 3;
constexpr const char* kVersionPaths[kVersions] = {"", "/console", "/go"};
constexpr const char* kVersionNames[kVersions] = {"Overall", "Console", "Go!"};
constexpr char kCategory[] = " - No Chicken Glitchless";  // after the version on the page
constexpr uint32_t kTrueSkill = 0xFFFFFFFFu;  // the course id of the ranked-match board, and of no board types
constexpr auto kFresh = std::chrono::minutes(10);
constexpr auto kRetry = std::chrono::seconds(20);
constexpr auto kGiveUp = std::chrono::seconds(8);  // an enumeration never waits on the server longer

struct Row {
  std::string name;
  uint32_t time = 0;  // milliseconds
  uint32_t id = 0;    // of the name, in g_names
};

// ---- The cache -------------------------------------------------------------------

struct Board {
  bool answered = false;  // rows are the server's (possibly none)
  bool queued = false;
  Clock::time_point at;   // when it was last asked for
  std::vector<Row> rows;
};

std::mutex g_mutex;  // guards the cache and the queue
std::condition_variable g_wake;
std::map<uint32_t, Board> g_boards;  // by key: the version above the view id
std::vector<std::string> g_names;  // every name seen; a row carries its index in its XUID
std::map<std::string, uint32_t> g_name_ids;
std::deque<uint32_t> g_queue;
bool g_stop = false;
std::thread g_thread;

// The key of a view's board in a version, or 0 for a view the server has nothing for (the TrueSkill view), which stays
// empty.
uint32_t Key(uint32_t view, uint32_t version) {
  const bool known = (view >= 30 && view <= 59) || (view >= 119 && view <= 165);
  return known ? (version << 16) | view : 0;
}

// Asks the worker for a board that is missing or old; one the game is waiting on goes first.
void Want(uint32_t key, bool ahead = false) {
  if (!key) return;
  std::lock_guard lock(g_mutex);
  Board& board = g_boards[key];
  if (board.queued) {
    if (!ahead && std::erase(g_queue, key)) g_queue.push_front(key);  // not in the queue: being fetched
    return;
  }
  const auto age = Clock::now() - board.at;
  if (board.answered ? age < kFresh : (board.at != Clock::time_point{} && age < kRetry)) return;
  board.queued = true;
  board.at = Clock::now();
  if (ahead) g_queue.push_back(key); else g_queue.push_front(key);
  g_wake.notify_all();
}

// The board's rows; false while the server has not answered for it yet.
bool Cached(uint32_t key, std::vector<Row>* rows) {
  rows->clear();
  if (!key) return true;
  std::lock_guard lock(g_mutex);
  const auto it = g_boards.find(key);
  if (it == g_boards.end() || !it->second.answered) return false;
  *rows = it->second.rows;
  return true;
}

// A name in the characters the game's gamertags have.
std::string Ascii(const std::string& name) {
  std::string out;
  for (const char c : name) out += (c >= 0x20 && c < 0x7F) ? c : '?';
  return out;
}

// The id of a name; g_mutex held.
uint32_t NameId(const std::string& name) {
  const auto [it, added] = g_name_ids.try_emplace(name, uint32_t(g_names.size()));
  if (added) g_names.push_back(name);
  return it->second;
}

void Worker() {
  std::unique_lock lock(g_mutex);
  while (!g_stop) {
    if (g_queue.empty()) {
      g_wake.wait(lock);
      continue;
    }
    const uint32_t key = g_queue.front();
    g_queue.pop_front();
    lock.unlock();
    const uint32_t view = key & 0xFFFF, version = (key >> 16) % kVersions;
    std::string text;
    const int status = online::Get("/v1/speedrun/" + std::to_string(view) + kVersionPaths[version], &text);
    std::vector<Row> rows;
    const nlohmann::json reply = nlohmann::json::parse(text, nullptr, false);
    if (status == 200 && reply.is_object() && reply.contains("rows") && reply["rows"].is_array()) {
      for (const nlohmann::json& row : reply["rows"]) {
        if (!row.is_object() || !row.contains("name") || !row["name"].is_string() || !row.contains("time") ||
            !row["time"].is_number_integer()) {
          continue;
        }
        const int64_t time = row["time"].get<int64_t>();
        if (time > 0 && time < 0x7FFFFFFF) rows.push_back({Ascii(row["name"].get<std::string>()), uint32_t(time)});
      }
    }
    if (status == 200) {
      REXLOG_DEBUG("[boards] board {} ({}): {} rows", view, kVersionNames[version], rows.size());
    } else {
      REXLOG_WARN("[boards] board {} ({}): the server answered {}", view, kVersionNames[version], status);
    }
    lock.lock();
    Board& board = g_boards[key];
    board.queued = false;
    // A board that could not be fetched keeps what it had and is asked for again later.
    if (status == 200 || status == 404) {
      for (Row& row : rows) row.id = NameId(row.name);
      board.answered = true;
      board.rows = std::move(rows);
    }
  }
}

// ---- Results in the game's format ------------------------------------------------

struct Query {
  uint32_t view = 0;
  uint32_t version = 0;
  uint16_t column = 0;
  bool by_rank = false;
  uint32_t start = 1;  // by rank: the first rank wanted
  uint32_t rows = 0;
};

template <typename T>
T* Guest(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<T*>(address);
}
void Put32(uint32_t address, uint32_t value) { rex::memory::store_and_swap<uint32_t>(Guest<uint8_t>(address), value); }

// An XUID for a name, kept clear of the local player's low half, which the game takes for the player's own row.
uint64_t RowXuid(uint32_t id) {
  uint32_t low = kXuidBase + id;
  const uint32_t live = uint32_t(online::Xuid()), local = uint32_t(REX_KERNEL_STATE()->user_profile()->xuid());
  while (low == live || low == local) low += kXuidStep;
  return (uint64_t(kXuidHigh) << 32) | low;
}

// Where this module last wrote rows: the game copies the names out of these.
struct Region {
  uint32_t rows = 0, count = 0;
};
Region g_regions[4];  // guarded by g_mutex
size_t g_next_region = 0;

void Remember(uint32_t rows_ptr, uint32_t count) {
  std::lock_guard lock(g_mutex);
  for (Region& region : g_regions) {
    if (region.rows != rows_ptr) continue;
    region.count = count;
    return;
  }
  g_regions[g_next_region++ % std::size(g_regions)] = {rows_ptr, count};
}

// The whole name of the row whose gamertag field is at 'tag'; false when it is no row of this module's.
bool FullName(uint32_t tag, std::string* name) {
  std::lock_guard lock(g_mutex);
  for (const Region& region : g_regions) {
    const uint32_t offset = tag - region.rows;
    if (!region.count || tag < region.rows || offset % kRowSize != kTagOffset || offset / kRowSize >= region.count) {
      continue;
    }
    const uint8_t* row = Guest<uint8_t>(tag - kTagOffset);
    const char* field = reinterpret_cast<const char*>(row) + kTagOffset;
    const uint64_t xuid = rex::memory::load_and_swap<uint64_t>(row);
    const uint32_t id = (uint32_t(xuid) & (kXuidStep - 1)) - kXuidBase;
    if (uint32_t(xuid >> 32) != kXuidHigh || id >= g_names.size()) return false;
    const std::string& full = g_names[id];
    const size_t length = strnlen(field, kTagSize + 1);
    if (length != std::min<size_t>(full.size(), kTagSize) || full.compare(0, length, field, length) != 0) return false;
    *name = full;
    return true;
  }
  return false;
}

// XUSER_STATS_ROW and its one XUSER_STATS_COLUMN: ranks count up from 1 with no gaps, as the game's paging needs.
void PutRow(uint32_t row_ptr, uint32_t column_ptr, uint32_t rank, const Row& row, uint16_t column) {
  uint8_t* out = Guest<uint8_t>(row_ptr);
  std::memset(out, 0, kRowSize);
  rex::memory::store_and_swap<uint64_t>(out, RowXuid(row.id));
  rex::memory::store_and_swap<uint32_t>(out + 8, rank);
  rex::memory::store_and_swap<uint64_t>(out + 0x10, row.time);
  std::memcpy(out + kTagOffset, row.name.data(), std::min<size_t>(row.name.size(), kTagSize));
  rex::memory::store_and_swap<uint32_t>(out + 0x28, 1);
  rex::memory::store_and_swap<uint32_t>(out + 0x2C, column_ptr);
  uint8_t* value = Guest<uint8_t>(column_ptr);
  std::memset(value, 0, kColumnSize);
  rex::memory::store_and_swap<uint16_t>(value, column);
  value[8] = 1;  // XUSER_DATA_TYPE_INT32
  rex::memory::store_and_swap<uint32_t>(value + 0x10, row.time);
}

// XUSER_STATS_READ_RESULTS with one view at 'results'; its rows and columns start at 'rows_ptr'.
void PutView(uint32_t results, uint32_t rows_ptr, const Query& query, const std::vector<Row>& rows, uint32_t first,
             uint32_t count, uint32_t total) {
  Put32(results, 1);
  Put32(results + 4, results + 8);
  Put32(results + 8, query.view);  // the game refuses a view that is not the one it asked for
  Put32(results + 12, total);
  Put32(results + 16, count);
  Put32(results + 20, rows_ptr);  // read even when there are no rows
  for (uint32_t i = 0; i < count; ++i) {
    PutRow(rows_ptr + i * kRowSize, rows_ptr + count * kRowSize + i * kColumnSize, first + i + 1, rows[first + i],
           query.column);
  }
  Remember(rows_ptr, count);
}

// ---- The enumerations ------------------------------------------------------------

// Game thread only from here on.
std::map<uint32_t, Query> g_queries;  // by the handle given to the game
uint32_t g_version = 0;               // what the page shows; X steps it
uint32_t g_next_handle = 0xCC1B0000u;

struct Pending {
  uint32_t overlapped = 0;  // 0: none
  uint32_t buffer = 0, size = 0;
  Query query;
  Clock::time_point since;
} g_pending;

// Answers an enumeration into the game's buffer.
void Fill(uint32_t buffer, uint32_t size, const Query& query, const std::vector<Row>& rows) {
  std::memset(Guest<uint8_t>(buffer), 0, size);
  const uint32_t total = query.by_rank ? uint32_t(rows.size()) : 0;  // the local player is on no board
  const uint32_t first = std::min(std::max<uint32_t>(query.start, 1) - 1, total);
  const uint32_t room = size > kHeaderSize ? (size - kHeaderSize) / (kRowSize + kColumnSize) : 0;
  const uint32_t count = std::min({total - first, query.rows, room});
  PutView(buffer, buffer + kHeaderSize, query, rows, first, count, total);
}

// XamUserCreateStatsEnumerator(title, kind, pivot, rows, specs count, specs, &buffer size, &handle).
void CreateEnumerator(PPCContext& ctx) {
  const uint32_t specs = ctx.r8.u32, size_ptr = ctx.r9.u32, handle_ptr = ctx.r10.u32;
  Query query;
  query.by_rank = ctx.r4.u32 == 1;
  query.start = query.by_rank ? uint32_t(ctx.r5.u64) : 1;
  query.rows = std::min<uint32_t>(ctx.r6.u32, 100);
  query.version = g_version;  // only the page enumerates
  if (specs && ctx.r7.u32) {
    query.view = rex::memory::load_and_swap<uint32_t>(Guest<uint8_t>(specs));
    query.column = rex::memory::load_and_swap<uint16_t>(Guest<uint8_t>(specs + 8));
  }
  if (g_queries.size() >= 64) g_queries.erase(g_queries.begin());  // the game never closes these
  const uint32_t handle = g_next_handle++;
  g_queries[handle] = query;
  Want(Key(query.view, query.version));
  if (size_ptr) Put32(size_ptr, kHeaderSize + query.rows * (kRowSize + kColumnSize));
  if (handle_ptr) Put32(handle_ptr, handle);
  ctx.r3.u64 = 0;
}

u32 Enumerate(u32 handle, u32 flags, mapped_void buffer, u32 buffer_length, mapped_u32 items_returned,
              ppc_ptr_t<rex::system::XAM_OVERLAPPED> overlapped) {
  const auto it = g_queries.find(handle);
  if (it == g_queries.end()) {
    return rex::kernel::xam::XamEnumerate_entry(handle, flags, buffer, buffer_length, items_returned, overlapped);
  }
  std::vector<Row> rows;
  if (!overlapped) {
    Cached(Key(it->second.view, it->second.version), &rows);
    Fill(buffer.guest_address(), buffer_length, it->second, rows);
    return 0;
  }
  // Left pending until the board is in; Tick completes it.
  g_pending = {overlapped.guest_address(), buffer.guest_address(), buffer_length, it->second, Clock::now()};
  overlapped->result = kIoPending;
  overlapped->length = 0;
  Want(Key(it->second.view, it->second.version));
  return kIoPending;
}

// The game cancels before it frees a request's buffer, so a cancelled request must never be completed.
u32 CancelRequest(ppc_ptr_t<rex::system::XAM_OVERLAPPED> overlapped, u32 wait) {
  if (overlapped && overlapped.guest_address() == g_pending.overlapped) {
    g_pending = {};
    return 0;
  }
  return rex::kernel::xam::XMsgCancelIORequest_entry(overlapped, wait);
}

// Handles the game's swprintf when it copies a row's gamertag into its entry by writing the whole name, and returns
// false for any other call.
bool CopyName(PPCContext& ctx) {
  std::string name;
  if (ctx.r4.u32 != kNameFormat || !FullName(ctx.r5.u32, &name)) return false;
  const uint32_t length = uint32_t(std::min<size_t>(name.size(), kNameRoom));
  uint8_t* out = Guest<uint8_t>(ctx.r3.u32);
  for (uint32_t i = 0; i <= length; ++i) {
    rex::memory::store_and_swap<uint16_t>(out + 2 * i, i < length ? uint8_t(name[i]) : 0);
  }
  ctx.r3.u64 = length;
  return true;
}

bool g_stepping = false;  // game thread: LB or RB is changing the course

// LoadLeaderboard hook: the page's opening request for the friends filter gets overall instead, and false means the
// load is skipped.
bool BeforeLoad(uint32_t netman, PPCContext& ctx, const uint8_t* base) {
  if (g_stepping && ctx.r4.u32 == kTrueSkill) return false;
  if (ctx.r5.u32 == kFriends && !base[netman + kCentered]) {
    ctx.r5.u64 = kOverall;
    Put32(netman + kFilter, kOverall);
    g_version = 0;
  }
  return true;
}

// LB and RB: the TrueSkill board sits between the last course and the first, so a step onto it is taken again.
void StepCourse(PPCContext& ctx, uint8_t* base, PPCFunc* step) {
  const uint64_t result = ctx.r3.u64, netman = ctx.r4.u64, arguments = ctx.r5.u64;
  g_stepping = true;
  step(ctx, base);
  g_stepping = false;
  if (rex::memory::load_and_swap<uint32_t>(base + uint32_t(netman) + kCourse) != kTrueSkill) return;
  ctx.r3.u64 = result;
  ctx.r4.u64 = netman;
  ctx.r5.u64 = arguments;
  step(ctx, base);
}

// X: the game steps its filter and loads, so the filter is set to my score (overall comes next) and the last view is
// cleared to make the load start over for the next version.
void StepVersion(uint32_t netman, const uint8_t* base) {
  if (rex::memory::load_and_swap<uint32_t>(base + netman + kOff)) return;
  g_version = (g_version + 1) % kVersions;
  Put32(netman + kFilter, kMyScore);
  Put32(netman + kLastView, 0xFFFFFFFFu);
}

// Replaces the page's label (the game's "Overall - All Times") with the version and the speedrun.com category.
void Label(PPCContext& ctx, uint8_t* base) {
  const uint32_t netman = ctx.r3.u32, out = ctx.r5.u32;
  if (ctx.r4.u32 == kTrueSkill || base[netman + kCentered]) {
    __imp__sub_8297EEC0(ctx, base);
    return;
  }
  const std::string label = std::string(kVersionNames[g_version]) + kCategory;
  for (size_t i = 0; i <= label.size(); ++i) {
    rex::memory::store_and_swap<uint16_t>(base + out + 2 * i, i < label.size() ? uint8_t(label[i]) : 0);
  }
}

// Y changes nothing on the course with no board types (-1), so the course reads as that for the length of the call.
uint32_t HideCourse(uint32_t netman, const uint8_t* base) {
  const uint32_t course = rex::memory::load_and_swap<uint32_t>(base + netman + kCourse);
  Put32(netman + kCourse, kTrueSkill);
  return course;
}

void ShowCourse(uint32_t netman, uint32_t course) { Put32(netman + kCourse, course); }

}  // namespace

void Initialize() { g_thread = std::thread(Worker); }

void Shutdown() {
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_wake.notify_all();
  if (g_thread.joinable()) g_thread.join();
}

void Tick() {
  // Level select and the tally read a board once and show what is cached, so every course board is fetched ahead.
  static bool warmed = false;
  if (!warmed && online::Live()) {
    warmed = true;
    for (uint32_t view = 30; view <= 44; ++view) Want(Key(view, 0), true);
    for (uint32_t view = 119; view <= 149; ++view) {
      if (view != 130) Want(Key(view, 0), true);
    }
  }
  if (!g_pending.overlapped) return;
  std::vector<Row> rows;
  const uint32_t key = Key(g_pending.query.view, g_pending.query.version);
  if (!Cached(key, &rows) && Clock::now() - g_pending.since < kGiveUp) return;
  const Pending done = std::exchange(g_pending, {});
  Fill(done.buffer, done.size, done.query, rows);
  auto* overlapped = Guest<rex::system::XAM_OVERLAPPED>(done.overlapped);
  overlapped->extended_error = 0;
  overlapped->length = 0;
  overlapped->result = 0;  // last: the game reads the buffer once this is not pending
}

bool FriendsView(uint32_t specs_count, uint32_t specs_ptr, uint32_t results_size, uint32_t results_ptr) {
  if (!specs_count || !specs_ptr || !results_ptr || results_size < kHeaderSize) return false;
  Query query;
  query.view = rex::memory::load_and_swap<uint32_t>(Guest<uint8_t>(specs_ptr));
  query.column = rex::memory::load_and_swap<uint16_t>(Guest<uint8_t>(specs_ptr + 8));
  std::vector<Row> rows;
  if (!Cached(Key(query.view, 0), &rows)) Want(Key(query.view, 0));
  // The caller's buffer has room for the rows it asked about only, so the board's go in a block of this module's;
  // the game copies them out on its next frame.
  static uint32_t block = 0;
  constexpr uint32_t kBlockSize = kFriendRows * (kRowSize + kColumnSize);
  if (!block) block = REX_KERNEL_MEMORY()->SystemHeapAlloc(kBlockSize);
  if (!block) return false;
  std::memset(Guest<uint8_t>(block), 0, kBlockSize);
  std::memset(Guest<uint8_t>(results_ptr), 0, results_size);
  const uint32_t count = std::min<uint32_t>(uint32_t(rows.size()), kFriendRows);
  PutView(results_ptr, block, query, rows, 0, count, count);
  return true;
}

}  // namespace cc1::boards

REX_HOOK_RAW(sub_821B09B8) {
  if (!cc1::boards::CopyName(ctx)) __imp__sub_821B09B8(ctx, base);
}

REX_HOOK_RAW(sub_829801D8) {
  if (cc1::boards::BeforeLoad(ctx.r3.u32, ctx, base)) __imp__sub_829801D8(ctx, base);
}

REX_HOOK_RAW(sub_829844A0) { cc1::boards::StepCourse(ctx, base, __imp__sub_829844A0); }
REX_HOOK_RAW(sub_82984590) { cc1::boards::StepCourse(ctx, base, __imp__sub_82984590); }

REX_HOOK_RAW(sub_829843F0) {
  cc1::boards::StepVersion(ctx.r4.u32, base);
  __imp__sub_829843F0(ctx, base);
}

REX_HOOK_RAW(sub_8297EEC0) { cc1::boards::Label(ctx, base); }

REX_HOOK_RAW(sub_82984698) {
  const uint32_t netman = ctx.r4.u32;
  const uint32_t course = cc1::boards::HideCourse(netman, base);
  __imp__sub_82984698(ctx, base);
  cc1::boards::ShowCourse(netman, course);
}

REX_HOOK_RAW(__imp__XamUserCreateStatsEnumerator) { cc1::boards::CreateEnumerator(ctx); }
REX_HOOK(__imp__XamEnumerate, cc1::boards::Enumerate)
REX_HOOK(__imp__XMsgCancelIORequest, cc1::boards::CancelRequest)
