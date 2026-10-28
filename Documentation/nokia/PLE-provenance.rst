===============================
Nokia 6 PLE kernel provenance
===============================

This directory is the Nokia-specific Linux 4.19 port boundary. It does not
inherit an active Nokia common tree and it must not carry another product's
identity, electrical data, firmware, or binary artifacts.

Kernel-Base: 704f6f08135c39728fcbe6f94b3956d911bb1801
HMD-Commit: d548dae4af7fa5a75be88ce2e93f95ad4c867777
HMD-Tree: cafcf6d3475a0fd336f5766178110eee249dc397

The kernel base provides the Linux 4.19.325 and modern Qualcomm/Android API
substrate. The exact HMD Android 9 source provides Nokia D1C hardware identity
and board data. Source adoption after K00 must name the exact source commit and
path in the importing commit message. The Xiaomi/Mi-Thorium histories are
interface references only and are never Nokia hardware authorities.

K00 intentionally adds only build plumbing and four empty DTS shells. Hardware
nodes are introduced by later, subsystem-bounded commits after explicit
3.18-to-4.19 binding translation and compile validation.
