# agentOS

Custom OS for RISC-V RV64GC (QEMU virt / StarFive Star64). Agents declare a goal, urgency, and deadline instead of just being scheduled processes. The kernel uses that information directly.

Every agent says what it's trying to accomplish. The scheduler boosts priority as deadlines approach. Failures propagate up the goal hierarchy as typed messages so parent agents can spawn recovery children. An interactive shell lets you spawn, kill, and observe agents at runtime.

```
# Requires riscv64-unknown-elf-gcc and qemu-system-riscv64
# macOS: brew install riscv-gnu-toolchain qemu

make run
```

Boots via OpenSBI, detects RAM from DTB, initializes a 128MB heap, and drops into the shell.

```
agentOS> blueprints
agentOS> spawn demo
agentOS> tree
agentOS> ps
```

Inspired by Microsoft Project Solaris.
