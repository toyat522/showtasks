# showtasks

Show and complete incomplete tasks in Obsidian through the terminal.
Tasks must be created via the [Tasks](https://community.obsidian.md/plugins/obsidian-tasks-plugin)
plugin in Obsidian.

Requires "Scheduled" date in the Obsidian task. Matches output with

```tasks
not done 
scheduled on or before today
```

in Obsidian.

# Dependencies
- [lolcat](https://github.com/busyloop/lolcat)
- [ripgrep](https://github.com/burntsushi/ripgrep)

# Build
```
mkdir build
cd build
cmake ..
make
```

# Usage

## Show all incomplete tasks
`showtasks`
