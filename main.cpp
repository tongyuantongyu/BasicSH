#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <memory>

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

std::unordered_set<std::string> bb_commands;

const char* BUSYBOX = "./.runtime/busybox";
const char* BUSYBOX_LIST = "./.runtime/busybox --list";

const char* HELP_STRING = R"__HELP__(Usage Help

    BasicSH can run commands like any shell.

  Special characters

    You can use 'single' or "double" quotes to quote input to let it interpreted as a single argument.
    Backslash\ can be used to break your command into multi lines, or to input newline or tab character.

  Meta commands

    To exit, use meta command @exit.
    To show this help, use meta command @help.
    To start a process but ignore its output, use meta command @start.

  Portable Coreutils Support

    BasicSH supports portable coreutils by busybox. Busybox should be
    placed at .runtime/busybox related to current directory to be found
    by BasicSH. BasicSH will detect available commands provided by busybox,
    and prior to busybox version than system version of a command.
)__HELP__";

int null_fd;
int shell_pid;

void get_bb_commands() {
  int o_stderr = dup(STDERR_FILENO);
  dup2(null_fd, STDERR_FILENO);

  FILE* pipe = popen(BUSYBOX_LIST, "r");

  dup2(o_stderr, STDERR_FILENO);

  if (errno) {
    std::cerr << "Warning: Busybox not available.";
    std::cout << std::endl;
    std::cout.flush();
    pclose(pipe);
    return;
  }

  char buffer[128];
  std::stringstream stream;

  while (fgets(buffer, 128, pipe) != nullptr) {
    stream << buffer;
  }

  pclose(pipe);

  std::string tmp;
  while (!stream.eof()) {
    stream >> tmp;
    if (!tmp.empty()) {
      bb_commands.insert(tmp);
    }
  }

  if (bb_commands.empty()) {
    std::cerr << "Warning: Busybox not available.";
    std::cout << std::endl;
    return;
  }
  
  std::cout << bb_commands.size() << " commands loaded from busybox." << std::endl;
}

class CommandReader {
 public:
  bool pristine;
  bool escape;
  bool enclosed;
  enum {
    STATE_NORMAL,
    STATE_QUOTE_SINGLE,
    STATE_QUOTE_DOUBLE
  } state;
  std::string executable;
  std::vector<std::string> arguments;
  std::stringstream buffer;

  CommandReader() { // NOLINT(cppcoreguidelines-pro-type-member-init)
    this->reset();
  }

  void put(const std::string &input) {
    if (this->enclosed) {
      return;
    }

    if (this->state != STATE_NORMAL) {
      this->putchar('\n');
    } else if (!this->pristine) {
      this->escape = false;
    }

    for (const auto& c : input) {
      this->putchar(c);
    }

    this->pristine = false;
  }

  bool can_enclose() const {
    return !this->enclosed && this->state == STATE_NORMAL && !this->escape;
  }

  bool enclose() {
    if (!this->can_enclose()) {
      return false;
    }

    submit_buffer();
    this->enclosed = true;
    return true;
  }

  const char *exec() const {
    if (!this->enclosed) {
      return nullptr;
    }

    return this->executable.c_str();
  }

  std::unique_ptr<char *[]> args() const {
    if (!this->enclosed) {
      auto empty = std::unique_ptr<char *[]>();
      return empty;
    }

    auto args = std::make_unique<char *[]>(this->arguments.size() + 1);

    for (size_t i = 0; i < this->arguments.size(); i++) {
      args[i] = (char *)this->arguments[i].c_str();
    }

    args[this->arguments.size()] = nullptr;

    return args;
  }

  void clear() {
    this->reset();
    this->executable.clear();
    this->arguments.clear();
    this->buffer.str("");
    this->buffer.clear();
  }

 private:
  void putchar(const char& c) {
    if (this->escape) {
      switch (c) {
        case 'n':
          this->buffer.put('\n');
          break;
        case 't':
          this->buffer.put('\t');
          break;
        default:
          this->buffer.put(c);
      }
      this->escape = false;
      return;
    }

    switch (this->state) {
      case STATE_NORMAL:
        switch (c) {
          case '\\':
            this->escape = true;
            break;
          case '"':
            this->state = STATE_QUOTE_DOUBLE;
            break;
          case '\'':
            this->state = STATE_QUOTE_SINGLE;
            break;
          case '\t':
          case '\n':
          case ' ':
            this->submit_buffer();
            break;
          default:
            this->buffer.put(c);
        }
        break;

      case STATE_QUOTE_SINGLE:
        switch (c) {
          case '\\':
            this->escape = true;
            break;
          case '\'':
            this->state = STATE_NORMAL;
            break;
          default:
            this->buffer.put(c);
        }
        break;

      case STATE_QUOTE_DOUBLE:
        switch (c) {
          case '\\':
            this->escape = true;
            break;
          case '"':
            this->state = STATE_NORMAL;
            break;
          default:
            this->buffer.put(c);
        }
        break;
    }
  }

  void submit_buffer() {
    auto content = this->buffer.str();
    if (!content.empty()) {
      if (this->executable.empty()) {
        if (bb_commands.find(content) != bb_commands.end()) {
          this->executable = BUSYBOX;
          this->arguments.push_back(std::move(content));
        } else {
          this->arguments.push_back(content);
          this->executable.swap(content);
        }
      } else {
        this->arguments.push_back(std::move(content));
      }
    }

    this->buffer.str("");
    this->buffer.clear();
  }

  void reset() {
    this->pristine = true;
    this->escape = false;
    this->enclosed = false;
    this->state = STATE_NORMAL;
  }
};

int run(const CommandReader &command, const bool wait) {
  int pid = fork();

  if (pid != 0) {
    // shell process
    return pid;
  }

  // command process
  int fd_null;

  if (!wait) {
    // we don't wait, so don't mess the shell
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
  }

  if (execvp(command.exec(), command.args().get()) == -1) {
    std::cerr << "Failed execute command.";
    std::cout << std::endl;
    std::cout.flush();
  }

  exit(0);
}

const std::string META_EXIT  = "exit",
                  META_START = "start",
                  META_HELP = "help";

inline bool isCommand(const std::string& input, const std::string& command) {
  return std::mismatch(++input.begin(), input.end(), command.begin(), command.end()).second == command.end();
}

void open_null() {
  null_fd = open("/dev/null", O_WRONLY | O_CREAT, 0666);
  shell_pid = getpid();
}

void close_null() {
  if (getpid() == shell_pid) {
    close(null_fd);
  }
}

int main() {
  open_null();
  atexit(&close_null);
  std::cout << "Basic SH" << std:: endl;
  std::cout << "Enter @help for usage help." << std:: endl;

  get_bb_commands();

  bool begin = true;
  bool wait = true;
  CommandReader r;

  while (true) {
    if (begin) {
      std::cout << "B $> ";
    } else {
      std::cout << "> ";
    }

    std::string input;
    std::getline(std::cin, input);

    if (begin && *input.begin() == '@') {
      if (isCommand(input, META_EXIT)) {
        break;
      } else if (isCommand(input, META_START)) {
        input = input.substr(META_START.size() + 1);
        r.put(input);
        wait = false;
      } else if (isCommand(input, META_HELP)) {
        std::cout << HELP_STRING;
        std::cout.flush();
      } else {
          std::cerr << "Bad Meta command.";
          std::cout << std::endl;
          std::cout.flush();
      }
    } else {
      r.put(input);
      wait = true;
    }

    if (r.can_enclose()) {
      r.enclose();

      if (!r.executable.empty()) {
        int pid = run(r, wait);
        if (pid > 0) {
          if (wait) {
            waitpid(pid, nullptr, 0);
          }
        } else {
          std::cerr << "Failed execute command.";
          std::cout << std::endl;
          std::cout.flush();
        }
      }

      r.clear();
      begin = true;
    } else {
      begin = false;
    }
  }

  return 0;
}
