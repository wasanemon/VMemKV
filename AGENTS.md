- Do not `git commit` or `git push`
- Following the rules:
    - YAGNI
    - DRY
    - KISS
- When creating a documentation, follows the `diataxis` framework.
- Use **Spot** Instances for the AWS EC2 instances to reduce costs.
- Check remaining AWS EC2 instances and terminate them periodically to avoid unnecessary costs.
- Do not consume too much disk space in the local machine.
- Source code comments and design docs (e.g. `low_level_design.md`) must be declarative: describe
  the current system only. Never narrate history, rationale, or process ("we had X, measured Y,
  removed it because Z") inside them — that belongs in the commit message, not the file.
    - When removing a feature/tag/config, delete it and its comments outright. Do not leave a
      comment behind explaining that something no longer exists or why it was removed.
    - If the removal's rationale is genuinely worth preserving beyond the commit message, do not
      write it into source/docs unprompted — propose a separate, dedicated decision-record
      document and get the user's go-ahead before creating it.
- Never diagnose a performance bottleneck, or propose/discuss a design change to fix one, from
  code-reading or reasoning alone. Measure first (add timers, profile, break down the call into
  its component phases) and let the numbers name the actual bottleneck before proposing a fix —
  a plausible-sounding hypothesis about where time goes can be confidently wrong in exactly the
  part that matters, and the real cost can sit somewhere structurally unrelated to what looked
  suspicious from the code alone.