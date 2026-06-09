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
agentOS> delegate check spacecraft health
agentOS> delegations
agentOS> tree
agentOS> ps
```

You don't have to name a blueprint. `delegate` hands a goal to the task manager, which matches the words against each blueprint's declared skills and dispatches the best fit. The result comes back to the shell asynchronously.

The dispatcher watches for trigger conditions and activates agents on its own — by default any agent failure spawns a triage agent that prints a post-mortem of the failed agent's goal, contract, and progress. `rules` shows the active triggers.

Inspired by Microsoft Project Solaris.
