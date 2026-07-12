# Prefix Google Benchmark output with wall clock time, elapsed time, and
# progress counters.
#
# Expected state file format:
# - "run_label run_total global_total"

function color_enabled() {
  return ("VMEMKV_COLOR" in ENVIRON) && ENVIRON["VMEMKV_COLOR"] != "" && ENVIRON["VMEMKV_COLOR"] != "0" && tolower(ENVIRON["VMEMKV_COLOR"]) != "false";
}

function load_state(   line) {
  global_total = 0;

  if ((getline line < state_file) > 0) {
    global_total = line + 0;
  }
  close(state_file);
}

function make_prefix(global_done, global_total,   now, wall, elapsed, hrs, mins, secs, percent) {
  now = systime();
  wall = strftime("%H:%M:%S", now);
  elapsed = now - start;
  hrs = int(elapsed / 3600);
  mins = int((elapsed % 3600) / 60);
  secs = elapsed % 60;
  if (global_total > 0) {
    percent = int((global_done * 100) / global_total);
    return sprintf("[%s][%02d:%02d:%02d][Total %d/%d (%d%%)]", wall, hrs, mins, secs, global_done, global_total, percent);
  } else {
    return sprintf("[%s][%02d:%02d:%02d][Total %d]", wall, hrs, mins, secs, global_done);
  }
}

BEGIN {
  start = systime();
  last_emit = start;
  global_done = 0;
  use_color = color_enabled();
  reset = use_color ? "\033[0m" : "";
  prefix_color = use_color ? "\033[36m" : "";
  alert_color = use_color ? "\033[1;4m" : "";
  line_color = use_color ? "\033[32m" : "";
  header_color = use_color ? "\033[1m" : "";
  load_state();
}

{
  load_state();
  if ($0 ~ /Elapsed_Sec=/) {
    global_done++;
  }

  prefix = make_prefix(global_done, global_total);
  now = systime();
  style = "";
  marker = "";
  if (use_color && (now - last_emit) >= 60) {
    style = alert_color;
    marker = "[!slow!] ";
  }

  if ($0 ~ /^Store=VMemKV\//) {
    printf "%s%s%s%s %s%s%s\n", prefix_color, style, marker, prefix, reset, line_color, $0, reset;
  } else if (use_color) {
    printf "%s%s%s%s %s%s\n", prefix_color, style, marker, prefix, reset, $0;
  } else {
    printf "%s%s %s\n", marker, prefix, $0;
  }
  last_emit = now;
  fflush();
}
