#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace color {
const char *reset = "\033[0m";
const char *red = "\033[31m";
const char *yellow = "\033[33m";
const char *green = "\033[32m";
const char *dim = "\033[2m";
const char *cyan = "\033[36m";
const char *magenta = "\033[35m";
} // namespace color

struct Task {
  std::string text;
  std::string raw_text; // text with metadata still intact, used to rebuild the next occurrence
  std::string file;
  int line = 0;
  std::optional<std::string> due;
  std::optional<std::string> scheduled;
  std::optional<std::string> start;
  int priority = 3; // 6=highest .. 1=lowest, 3=none
  bool recurring = false;
  std::optional<std::string> recurrence_rule; // e.g. "every 2 days when done"
  std::optional<std::string> completion_flag; // "delete" or "keep", from 🏁
};

// A parsed "every N <unit>(s) [when done]" recurrence rule
struct RecurrenceRule {
  int interval;
  char unit; // 'd', 'w', 'm', 'y'
  bool when_done;
};

// Year, month, date
struct Ymd {
  int y, m, d;
};

// Get today's date
static std::string get_today_date() {
  std::time_t t = std::time(nullptr);
  std::tm tm = *std::localtime(&t);
  char buf[11];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return buf;
}

// Remove token (e.g. emoji) from string
static void strip_token(std::string &s, const std::string &token) {
  size_t pos;
  while ((pos = s.find(token)) != std::string::npos) {
    s.erase(pos, token.length());
  }
}

// Extract date after emoji and remove emoji + date from input desc
static std::optional<std::string> extract_date(std::string &desc, const std::string &emoji) {
  static const std::regex date_re(R"((\d{4}-\d{2}-\d{2}))");

  // Find given emoji
  size_t pos = desc.find(emoji);

  // If the emoji does not exist, there is no date
  if (pos == std::string::npos) {
    return std::nullopt;
  }

  // Extract rest of the string
  std::string rest = desc.substr(pos + emoji.length());

  // Find date regex match and remove emoji + date from desc
  std::smatch m;
  if (std::regex_search(rest, m, date_re)) {
    std::string date = m[1].str();
    size_t end_pos = pos + emoji.length() + m.position(0) + m.length(0);
    desc.erase(pos, end_pos - pos);
    return date;
  }

  return std::nullopt;
}

// Extract priority and remove emoji from input desc
static int extract_priority(std::string &desc) {
  struct P {
    const char *emoji;
    int level;
  };

  // Emojis for the priorities
  static const std::vector<P> priorities = {
    {"\xf0\x9f\x94\xba", 6}, // 🔺 highest
    {"\xe2\x8f\xab", 5},     // ⏫ high
    {"\xf0\x9f\x94\xbc", 4}, // 🔼 medium
    {"\xf0\x9f\x94\xbd", 2}, // 🔽 low
    {"\xe2\x8f\xac", 1},     // ⏬ lowest
  };

  for (auto &p : priorities) {
    if (desc.find(p.emoji) != std::string::npos) {
      strip_token(desc, p.emoji);
      return p.level;
    }
  }
  return 3;
}

// Extract recurrence rule text (e.g. "every 2 days when done") and remove it from desc
static std::optional<std::string> extract_recurrence(std::string &desc) {
  // Find recurrence emoji in desc
  const std::string emoji = "\xf0\x9f\x94\x81"; // 🔁
  size_t pos = desc.find(emoji);
  if (pos == std::string::npos) {
    return std::nullopt;
  }

  // The rule text runs from the emoji to the next stop marker (or end of string)
  static const std::vector<std::string> stop_markers = {
    "\xf0\x9f\x93\x85", // 📅
    "\xe2\x8f\xb3",     // ⏳
    "\xf0\x9f\x9b\xab", // 🛫
    "\xf0\x9f\x8f\x81", // 🏁
    "\xf0\x9f\x86\x94", // 🆔
    "\xe2\x9b\x94",     // ⛔
    "\xe2\x9c\x85",     // ✅
    "\xe2\x9d\x8c",     // ❌
  };
  size_t end = desc.length();
  for (auto &marker : stop_markers) {
    size_t p = desc.find(marker, pos);
    if (p != std::string::npos && p < end) {
      end = p;
    }
  }

  size_t rule_start = pos + emoji.length();
  std::string rule = desc.substr(rule_start, end - rule_start);
  desc.erase(pos, end - pos);

  // Trim leftover leading/trailing spaces from the emoji/stop-marker boundaries
  size_t a = rule.find_first_not_of(' ');
  if (a == std::string::npos) {
    return std::nullopt;
  }
  size_t b = rule.find_last_not_of(' ');
  return rule.substr(a, b - a + 1);
}

// Erases emoji and the word after the emoji, returning that word
static std::string skip_word_after(std::string &desc, size_t marker_pos, size_t marker_len) {
  std::string rest = desc.substr(marker_pos + marker_len);
  size_t i = 0;
  while (i < rest.size() && rest[i] == ' ') {
    i++;
  }
  size_t j = i;
  while (j < rest.size() && rest[j] != ' ') {
    j++;
  }
  std::string word = rest.substr(i, j - i);
  while (j < rest.size() && rest[j] == ' ') {
    j++;
  }
  desc.erase(marker_pos, marker_len + j);
  return word;
}

// Extract and strip the "delete"/"keep" on-completion flag
static std::optional<std::string> extract_flag(std::string &desc) {
  const std::string completion = "\xf0\x9f\x8f\x81"; // 🏁
  size_t pos = desc.find(completion);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  return skip_word_after(desc, pos, completion.length());
}

// Collapse runs of 2+ spaces down to a single space
static void collapse_spaces(std::string &s) {
  static const std::regex multi_space(R"([ ]{2,})");
  s = std::regex_replace(s, multi_space, " ");
}

// Trim leading and trailing spaces/tabs
static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t");
  size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

// Wrap string in single quotes
static std::string shell_quote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

// Shells out to `rg` to find unchecked task lines
static std::vector<Task> collect_tasks(const fs::path &vault) {
  // ripgrep task text via regex replace
  std::string cmd = "cd " + shell_quote(vault.string()) +
                    " && rg --no-heading --line-number --no-column "
                    "--replace '' -e '^\\s*[-*+]\\s\\[ \\]\\s+' -g '*.md' .";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    std::cerr << "Failed to run rg\n";
    return {};
  }

  // One line of `rg`'s output: path:line:task_text
  static const std::regex rg_line_re(R"(^\.?/?([^:]+):(\d+):(.*)$)");

  std::vector<Task> tasks;
  std::array<char, 8192> buf;

  // Read one line at a time from the pipe into buf
  while (fgets(buf.data(), buf.size(), pipe)) {
    std::string raw(buf.data());

    // Strip trailing \n and \r characters
    while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) {
      raw.pop_back();
    }

    // Regex match the line
    std::smatch m;
    if (!std::regex_match(raw, m, rg_line_re)) {
      continue;
    }

    // Populate struct members from regex matched information
    Task task;
    task.file = m[1].str();
    task.line = std::stoi(m[2].str());
    std::string desc = m[3].str();
    task.raw_text = desc; // preserved with metadata intact, for rebuilding the next occurrence
    task.due = extract_date(desc, "\xf0\x9f\x93\x85");   // 📅
    task.scheduled = extract_date(desc, "\xe2\x8f\xb3"); // ⏳
    task.start = extract_date(desc, "\xf0\x9f\x9b\xab"); // 🛫
    task.priority = extract_priority(desc);
    task.recurrence_rule = extract_recurrence(desc);
    task.recurring = task.recurrence_rule.has_value();
    task.completion_flag = extract_flag(desc);
    collapse_spaces(desc);
    task.text = trim(desc);

    tasks.push_back(std::move(task));
  }
  pclose(pipe);
  return tasks;
}

// Filter away tasks not yet scheduled
static void filter_not_scheduled(std::vector<Task> &tasks, const std::string &today) {
  tasks.erase(
    std::remove_if(
      tasks.begin(),
      tasks.end(),
      [&](const Task &t) {
        return !t.scheduled.has_value() || *t.scheduled > today;
      }
    ),
  tasks.end());
}

static void sort_tasks(std::vector<Task> &tasks) {
  std::stable_sort(
    tasks.begin(),
    tasks.end(),
    [](const Task &a, const Task &b) {
      // If the priorities are different, higher priority should be shown first
      if (a.priority != b.priority) {
        return a.priority > b.priority;
      }

      // If the scheduled dates are different, prioritize earlier scheduled task
      if (a.scheduled.has_value() && b.scheduled.has_value() && *a.scheduled != *b.scheduled) {
        return *a.scheduled < *b.scheduled;
      }

      // Descending alphabetical order
      return a.text > b.text;
    }
  );
}

// Print given task with appropriate color and date
static void print_task(int i, const Task &t, const std::string &today) {
  std::ostringstream out;

  // Print text with color based on priority
  if (t.priority >= 5) {
    out << color::red << "!! ";
  } else if (t.priority == 4) {
    out << color::yellow << "! ";
  } else if (t.priority == 2 || t.priority == 1) {
    out << color::dim;
  }
  out << i << ": " << t.text << color::reset;

  // Print date with color based on scheduled date
  const char *c = "";
  if (*t.scheduled < today) {
    c = color::red;
  } else if (*t.scheduled == today) {
    c = color::yellow;
  } else {
    c = color::green;
  }
  out << " " << c << "[" << *t.scheduled << "]" << color::reset;

  // Print recurrence symbol if recurring
  if (t.recurring) {
    out << color::cyan << " \xf0\x9f\x94\x81" << color::reset;
  }

  std::cout << out.str() << "\n";
}

static Ymd parse_ymd(const std::string &s) {
  return Ymd{std::stoi(s.substr(0, 4)), std::stoi(s.substr(5, 2)), std::stoi(s.substr(8, 2))};
}

static std::string format_ymd(const Ymd &d) {
  char buf[11];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", d.y, d.m, d.d);
  return buf;
}

static bool is_leap_year(int y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in_month(int y, int m) {
  static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && is_leap_year(y)) {
    return 29;
  }
  return dim[m - 1];
}

static Ymd add_days(Ymd d, int n) {
  std::tm tm{};
  tm.tm_year = d.y - 1900;
  tm.tm_mon = d.m - 1;
  tm.tm_mday = d.d + n;
  tm.tm_hour = 12;
  std::mktime(&tm);
  return Ymd{tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

static Ymd add_months(Ymd d, int n) {
  int total = d.y * 12 + (d.m - 1) + n;
  int ny = total / 12;
  int nm = total % 12;
  if (nm < 0) {
    nm += 12;
    ny -= 1;
  }
  int nd = std::min(d.d, days_in_month(ny, nm + 1));
  return Ymd{ny, nm + 1, nd};
}

// Parses rule text like "every 2 days when done". Returns nullopt if the
// rule doesn't match this simple grammar (Tasks plugin supports a richer
// syntax, e.g. "every week on Monday", which isn't handled here).
static std::optional<RecurrenceRule> parse_recurrence(const std::string &rule_text) {
  static const std::regex re(
    R"(^every\s+(?:(\d+)\s+)?(day|week|month|year)s?(\s+when\s+done)?$)",
    std::regex::icase
  );
  std::smatch m;
  if (!std::regex_match(rule_text, m, re)) {
    return std::nullopt;
  }

  RecurrenceRule r;
  r.interval = m[1].matched ? std::stoi(m[1].str()) : 1;
  std::string unit = m[2].str();
  for (auto &c : unit) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (unit == "day") {
    r.unit = 'd';
  } else if (unit == "week") {
    r.unit = 'w';
  } else if (unit == "month") {
    r.unit = 'm';
  } else {
    r.unit = 'y';
  }
  r.when_done = m[3].matched;
  return r;
}

// Applies a recurrence rule to a "YYYY-MM-DD" date, returning the next date
static std::string apply_recurrence(const std::string &base_date, const RecurrenceRule &rule) {
  Ymd base = parse_ymd(base_date);
  switch (rule.unit) {
    case 'd':
      return format_ymd(add_days(base, rule.interval));
    case 'w':
      return format_ymd(add_days(base, rule.interval * 7));
    case 'm':
      return format_ymd(add_months(base, rule.interval));
    default:
      return format_ymd(add_months(base, rule.interval * 12));
  }
}

// Replaces the YYYY-MM-DD immediately following the given emoji marker
// within text with new_date. No-op if the emoji or a following date isn't found.
static void replace_date_after_emoji(std::string &text, const std::string &emoji, const std::string &new_date) {
  static const std::regex date_re(R"(\d{4}-\d{2}-\d{2})");
  size_t pos = text.find(emoji);
  if (pos == std::string::npos) {
    return;
  }
  std::string rest = text.substr(pos + emoji.length());
  std::smatch m;
  if (std::regex_search(rest, m, date_re)) {
    text.replace(pos + emoji.length() + m.position(0), m.length(0), new_date);
  }
}

// Builds the "- [ ] ..." line for the task's next occurrence.
// Returns nullopt if the task doesn't recur or its rule can't be parsed.
static std::optional<std::string> build_next_occurrence_line(
    const std::string &original_line, const Task &t, const std::string &today) {
  if (!t.recurring || !t.recurrence_rule) {
    return std::nullopt;
  }
  auto rule = parse_recurrence(*t.recurrence_rule);
  if (!rule) {
    std::cerr << "Warning: could not parse recurrence rule \"" << *t.recurrence_rule
              << "\"; not creating next occurrence\n";
    return std::nullopt;
  }

  // Apply recurrence rule from the base date
  std::string base_date = rule->when_done ? today : (t.scheduled ? *t.scheduled : today);
  std::string next_date = apply_recurrence(base_date, *rule);

  // Set new text with new date
  std::string new_text = t.raw_text;
  replace_date_after_emoji(new_text, "\xe2\x8f\xb3", next_date); // ⏳

  // Match original indentation level
  std::string indent;
  for (char c : original_line) {
    if (c == ' ' || c == '\t') {
      indent += c;
    } else {
      break;
    }
  }
  return indent + "- [ ] " + new_text;
}

// Marks the task done. If it recurs, the next occurrence is inserted.
// If its 🏁 flag is "delete," the completed line is replaced rather than kept.
static bool mark_task_done(const fs::path &vault, const Task &t, const std::string &today) {
  fs::path file_path = vault / t.file;
  std::ifstream in(file_path);
  if (!in) {
    std::cerr << "Failed to open file: " << file_path << "\n";
    return false;
  }

  // Append lines to vector
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  in.close();

  // Create next line if it is a recurring task
  int idx = t.line - 1;
  auto next_line = build_next_occurrence_line(lines[idx], t, today);
  bool delete_on_complete = t.completion_flag && *t.completion_flag == "delete";

  if (next_line && delete_on_complete) {
    // No checked copy is kept, so the line is replaced by the next occurrence
    lines[idx] = *next_line;
    std::ofstream out(file_path, std::ios::trunc);
    for (auto &l : lines) {
      out << l << "\n";
    }
    return true;
  }

  if (next_line) {
    // Insert the next occurrence above the completed line
    lines.insert(lines.begin() + idx, *next_line);
    idx += 1;
  }

  // Replace unchecked box with checked box and add done date
  std::string &target = lines[idx];
  size_t box = target.find("[ ]");
  target.replace(box, 3, "[x]");
  target += " \xe2\x9c\x85 " + today; // ✅ done date

  // Rewrite file
  std::ofstream out(file_path, std::ios::trunc);
  for (auto &l : lines) {
    out << l << "\n";
  }
  return true;
}

int main(int argc, char **argv) {
  const char *home = std::getenv("HOME");
  fs::path vault = fs::path(home ? home : "") / "docs" / "obsidian" / "Main";

  if (!fs::exists(vault)) {
    std::cerr << "Vault path does not exist: " << vault << "\n";
    return 1;
  }

  std::vector<Task> tasks = collect_tasks(vault);

  // Filter away tasks not yet scheduled and sort tasks
  std::string today = get_today_date();
  filter_not_scheduled(tasks, today);
  sort_tasks(tasks);

  // `showtasks done <task_number>`
  if (argc >= 3 && std::string(argv[1]) == "done") {
    int n = std::stoi(argv[2]);
    if (n < 0 || n >= static_cast<int>(tasks.size())) {
      std::cerr << "No task numbered " << n << "\n";
      return 1;
    }
    if (!mark_task_done(vault, tasks[n], today)) {
      return 1;
    }
    std::cout << "Marked done: " << tasks[n].text << "\n";
    return 0;
  }

  // `showtasks`: print tasks
  if (tasks.empty()) {
    int rc = std::system("echo '\xe2\xb8\x9c( \xc2\xb4 \xea\x92\xb3 ` )\xe2\xb8\x9d yay all tasks are completed "
                         "\xe2\xb8\x9c( \xc2\xb4 \xea\x92\xb3 ` )\xe2\xb8\x9d' | lolcat -F 0.5 -p 6 -S 8");
    return 0;
  }

  int i = 0;
  for (const auto &t : tasks) {
    print_task(i, t, today);
    i++;
  }

  return 0;
}
