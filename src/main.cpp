#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
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
  std::string file;
  int line = 0;
  std::optional<std::string> due;
  std::optional<std::string> scheduled;
  std::optional<std::string> start;
  int priority = 3; // 6=highest .. 1=lowest, 3=none
  bool recurring = false;
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

// Return true if recurring and remove recurrence from input desc
static bool extract_recurrence(std::string &desc) {
  // Find recurrence emoji in desc
  const std::string emoji = "\xf0\x9f\x94\x81"; // 🔁
  size_t pos = desc.find(emoji);
  if (pos == std::string::npos) {
    return false;
  }

  // Erase string between recurrence emoji and stop marker inclusive
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
  desc.erase(pos, end - pos);

  return true;
}

// Erases emoji and the word after the emoji
static void skip_word_after(std::string &desc, size_t marker_pos, size_t marker_len) {
  std::string rest = desc.substr(marker_pos + marker_len);
  size_t i = 0;
  while (i < rest.size() && rest[i] == ' ') {
    i++;
  }
  size_t j = i;
  while (j < rest.size() && rest[j] != ' ') {
    j++;
  }
  while (j < rest.size() && rest[j] == ' ') {
    j++;
  }
  desc.erase(marker_pos, marker_len + j);
}

// Strip delete/keep flag
static void strip_flag(std::string &desc) {
  const std::string completion = "\xf0\x9f\x8f\x81"; // 🏁
  size_t pos = desc.find(completion);
  if (pos != std::string::npos) {
    skip_word_after(desc, pos, completion.length());
  }
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
    task.due = extract_date(desc, "\xf0\x9f\x93\x85");   // 📅
    task.scheduled = extract_date(desc, "\xe2\x8f\xb3"); // ⏳
    task.start = extract_date(desc, "\xf0\x9f\x9b\xab"); // 🛫
    task.priority = extract_priority(desc);
    task.recurring = extract_recurrence(desc);
    strip_flag(desc);
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
static void print_task(const Task &t, const std::string &today) {
  std::ostringstream out;

  // Print text with color based on priority
  if (t.priority >= 5) {
    out << color::red << "!! ";
  } else if (t.priority == 4) {
    out << color::yellow << "! ";
  } else if (t.priority == 2 || t.priority == 1) {
    out << color::dim;
  }
  out << t.text << color::reset;

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

int main(int argc, char **argv) {
  const char *home = std::getenv("HOME");
  fs::path vault = fs::path(home ? home : "") / "docs" / "obsidian" / "Main";

  if (!fs::exists(vault)) {
    std::cerr << "Vault path does not exist: " << vault << "\n";
    return 1;
  }

  std::vector<Task> tasks = collect_tasks(vault);

  // Filter away tasks not yet scheduled
  std::string today = get_today_date();
  filter_not_scheduled(tasks, today);

  // Sort and print tasks
  sort_tasks(tasks);
  for (const auto &t : tasks) {
    print_task(t, today);
  }

  return 0;
}
