// A shell: tokenizing, fork/exec, pipelines, redirection, background jobs, signals.
//
//   g++ -std=c++23 -O2 shell.cpp -o shell && ./shell
//   ./shell --run "ls /etc | grep conf | wc -l"
//   ./shell --demo
//
// The mechanics people skip when they write a "shell" that just calls system():
// a pipeline needs N-1 pipes created before any child runs and every unused end
// closed in every process (miss one and the reader never sees EOF); background jobs
// need their own process group so Ctrl-C reaches the foreground job only; and the
// terminal must be handed to the foreground group with tcsetpgrp or interactive
// programs get stopped by SIGTTIN the moment they read.

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

struct Command {
    std::vector<std::string> argv;
    std::string input_file, output_file;
    bool append = false;
};

struct Pipeline {
    std::vector<Command> commands;
    bool background = false;
};

struct Job {
    pid_t pgid;
    std::string command;
    bool running = true;
    int id;
};

static std::vector<Job> g_jobs;
static int g_next_job_id = 1;
static pid_t g_shell_pgid;
static bool g_interactive = false;

// Tokenizing that respects quotes — "a b" is one argument, not two.
static std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_single = false, in_double = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\'' && !in_double) { in_single = !in_single; continue; }
        if (c == '"' && !in_single) { in_double = !in_double; continue; }
        if (c == '\\' && i + 1 < line.size() && !in_single) { current += line[++i]; continue; }
        if (!in_single && !in_double && std::strchr(" \t|<>&", c)) {
            if (!current.empty()) { tokens.push_back(current); current.clear(); }
            if (c == '>' && i + 1 < line.size() && line[i + 1] == '>') { tokens.push_back(">>"); ++i; }
            else if (!std::strchr(" \t", c)) tokens.push_back(std::string(1, c));
            continue;
        }
        current += c;
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

static Pipeline parse(const std::string& line) {
    Pipeline pipeline;
    Command command;
    auto tokens = tokenize(line);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        if (token == "|") { pipeline.commands.push_back(command); command = {}; }
        else if (token == "&") pipeline.background = true;
        else if (token == "<" && i + 1 < tokens.size()) command.input_file = tokens[++i];
        else if ((token == ">" || token == ">>") && i + 1 < tokens.size()) {
            command.output_file = tokens[++i];
            command.append = (token == ">>");
        } else command.argv.push_back(token);
    }
    if (!command.argv.empty() || !command.output_file.empty()) pipeline.commands.push_back(command);
    return pipeline;
}

static bool run_builtin(const Command& command) {
    if (command.argv.empty()) return false;
    const std::string& name = command.argv[0];
    if (name == "cd") {
        const char* target = command.argv.size() > 1 ? command.argv[1].c_str() : getenv("HOME");
        if (::chdir(target) != 0) std::perror("cd");
        return true;
    }
    if (name == "exit") { std::exit(command.argv.size() > 1 ? std::stoi(command.argv[1]) : 0); }
    if (name == "jobs") {
        for (const Job& job : g_jobs)
            std::printf("[%d] %s  %s\n", job.id, job.running ? "running" : "done   ", job.command.c_str());
        return true;
    }
    if (name == "fg") {
        if (g_jobs.empty()) { std::puts("fg: no current job"); return true; }
        Job& job = g_jobs.back();
        std::printf("%s\n", job.command.c_str());
        if (g_interactive) ::tcsetpgrp(STDIN_FILENO, job.pgid);
        ::kill(-job.pgid, SIGCONT);
        int status = 0;
        ::waitpid(-job.pgid, &status, WUNTRACED);
        if (g_interactive) ::tcsetpgrp(STDIN_FILENO, g_shell_pgid);
        g_jobs.pop_back();
        return true;
    }
    return false;
}

static int execute(const Pipeline& pipeline, const std::string& source) {
    if (pipeline.commands.empty()) return 0;
    // Flush before forking. A dirty stdio buffer is copied into every child, so
    // anything still sitting in it gets printed once per process — and unflushed
    // parent output would otherwise appear after output the children wrote directly.
    std::fflush(stdout);
    std::fflush(stderr);
    if (pipeline.commands.size() == 1 && run_builtin(pipeline.commands[0])) return 0;

    std::size_t count = pipeline.commands.size();
    std::vector<std::array<int, 2>> pipes(count > 1 ? count - 1 : 0);
    for (auto& p : pipes)
        if (::pipe(p.data()) < 0) { std::perror("pipe"); return 1; }

    pid_t pgid = 0;
    std::vector<pid_t> children;

    for (std::size_t i = 0; i < count; ++i) {
        pid_t pid = ::fork();
        if (pid < 0) { std::perror("fork"); return 1; }

        if (pid == 0) {
            // every child joins one process group, so a signal reaches the whole pipeline
            ::setpgid(0, pgid ? pgid : 0);
            if (!pipeline.background && g_interactive) ::tcsetpgrp(STDIN_FILENO, pgid ? pgid : ::getpid());
            for (int sig : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU}) ::signal(sig, SIG_DFL);

            if (i > 0) ::dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i + 1 < count) ::dup2(pipes[i][1], STDOUT_FILENO);
            for (auto& p : pipes) { ::close(p[0]); ::close(p[1]); }   // close EVERY end, or no EOF

            const Command& command = pipeline.commands[i];
            if (!command.input_file.empty()) {
                int fd = ::open(command.input_file.c_str(), O_RDONLY);
                if (fd < 0) { std::fprintf(stderr, "%s: %s\n", command.input_file.c_str(), std::strerror(errno)); ::_exit(1); }
                ::dup2(fd, STDIN_FILENO);
                ::close(fd);
            }
            if (!command.output_file.empty()) {
                int flags = O_WRONLY | O_CREAT | (command.append ? O_APPEND : O_TRUNC);
                int fd = ::open(command.output_file.c_str(), flags, 0644);
                if (fd < 0) { std::fprintf(stderr, "%s: %s\n", command.output_file.c_str(), std::strerror(errno)); ::_exit(1); }
                ::dup2(fd, STDOUT_FILENO);
                ::close(fd);
            }

            std::vector<char*> argv;
            for (const std::string& arg : command.argv) argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            std::fprintf(stderr, "%s: command not found\n", argv[0]);
            ::_exit(127);
        }

        if (!pgid) pgid = pid;
        ::setpgid(pid, pgid);          // set in the parent too: whoever gets there first wins the race
        children.push_back(pid);
    }

    for (auto& p : pipes) { ::close(p[0]); ::close(p[1]); }

    if (pipeline.background) {
        g_jobs.push_back({pgid, source, true, g_next_job_id++});
        std::printf("[%d] %d\n", g_jobs.back().id, pgid);
        return 0;
    }

    if (g_interactive) ::tcsetpgrp(STDIN_FILENO, pgid);
    int status = 0, last = 0;
    for (pid_t child : children) {
        ::waitpid(child, &status, WUNTRACED);
        if (WIFEXITED(status)) last = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) last = 128 + WTERMSIG(status);
        else if (WIFSTOPPED(status)) {
            g_jobs.push_back({pgid, source, false, g_next_job_id++});
            std::printf("\n[%d] stopped  %s\n", g_jobs.back().id, source.c_str());
            break;
        }
    }
    if (g_interactive) ::tcsetpgrp(STDIN_FILENO, g_shell_pgid);
    return last;
}

static void reap_background() {
    int status = 0;
    pid_t pid;
    while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0)
        for (Job& job : g_jobs)
            if (job.pgid == pid) {
                job.running = false;
                std::printf("[%d] done  %s\n", job.id, job.command.c_str());
            }
    g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [](const Job& j) { return !j.running; }),
                 g_jobs.end());
}

static int demo() {
    struct Case { const char* line; const char* what; };
    const Case cases[] = {
        {"echo hello from a shell I wrote", "argument splitting"},
        {"echo \"quoted   spaces stay together\"", "quote handling"},
        {"printf 'a\\nb\\nc\\n' | wc -l", "a two-stage pipeline"},
        {"printf 'delta\\nalpha\\ncharlie\\nbravo\\n' | sort | head -2", "three stages"},
        {"echo written-by-redirect > /tmp/shell-demo.txt", "output redirection"},
        {"cat < /tmp/shell-demo.txt", "input redirection"},
        {"echo appended >> /tmp/shell-demo.txt", "append redirection"},
        {"wc -l < /tmp/shell-demo.txt", "redirect + builtin-free pipeline"},
        {"nosuchcommand-xyz", "a command that does not exist"},
        {"sh -c 'exit 42'", "exit status propagation"},
    };
    for (const Case& c : cases) {
        std::printf("\n$ %s\n", c.line);
        std::fflush(stdout);
        int status = execute(parse(c.line), c.line);
        std::printf("  -> exit %d   (%s)\n", status, c.what);
    }

    std::printf("\n$ sleep 0.3 &\n");
    execute(parse("sleep 0.3 &"), "sleep 0.3 &");
    std::printf("  shell continues immediately; %zu job tracked\n", g_jobs.size());
    execute(parse("jobs"), "jobs");
    ::usleep(500'000);
    reap_background();

    std::puts("\n$ printf 'x\\ny\\nz\\n' | grep -v y | tr a-z A-Z | tee /tmp/shell-pipe.txt | wc -c");
    int status = execute(parse("printf 'x\\ny\\nz\\n' | grep -v y | tr a-z A-Z | tee /tmp/shell-pipe.txt | wc -c"),
                         "five-stage pipeline");
    std::printf("  -> exit %d   (five processes, four pipes, every unused end closed)\n", status);
    ::unlink("/tmp/shell-demo.txt");
    ::unlink("/tmp/shell-pipe.txt");
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--demo") return demo();
    if (argc > 2 && std::string(argv[1]) == "--run") return execute(parse(argv[2]), argv[2]);

    g_interactive = ::isatty(STDIN_FILENO);
    if (g_interactive) {
        while (::tcgetpgrp(STDIN_FILENO) != (g_shell_pgid = ::getpgrp())) ::kill(-g_shell_pgid, SIGTTIN);
        for (int sig : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU}) ::signal(sig, SIG_IGN);
        g_shell_pgid = ::getpid();
        ::setpgid(g_shell_pgid, g_shell_pgid);
        ::tcsetpgrp(STDIN_FILENO, g_shell_pgid);
    }

    std::string line;
    while (true) {
        reap_background();
        if (g_interactive) {
            char cwd[4096];
            ::getcwd(cwd, sizeof(cwd));
            std::printf("%s $ ", std::strrchr(cwd, '/') + 1);
            std::fflush(stdout);
        }
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        execute(parse(line), line);
    }
    return 0;
}
