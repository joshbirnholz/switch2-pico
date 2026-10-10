// The dongle's API data (core/src/web_api.c) and what the plugin needs from it.

export interface Link {
  state: string;
  addr: string;
  pid: number;
  battery_pct: number;
  charging: boolean;
}

export interface Status {
  version: string;
  platform: string;
  usb_mode: string;
  profile: string;
  usb_mouse: boolean;
  ctrl_type: number;
  pairing: { required: boolean; open: boolean; left_ms: number };
  links: Link[];
  bond: { max: number; list: { addr: string; pid: number }[] };
}

export interface Profile {
  name: string;
  mode: number;
  deadzone: number;
  outer: number;
  swap: number;
  threshold: number;
  rumble: number;
  strength: number;
  mouse: number;
  mouse_speed: number;
  mouse_flags: number;
  map: number[];
}

export interface CtrlType {
  name: string;
  numbered: boolean;
  active: number;
  profiles: (Profile | null)[];
  defaults: number[][];
}

export interface Settings {
  modes: { family: string; outputs: (string | null)[] }[];
  ctrl_type: number;
  types: CtrlType[];
  inputs: string[];
  quick_remap: number;
  usb_detach: number;
  decky_options?: number;
}

// Controller types (ctrl_type_t).
export const CTRL_PRO = 0, CTRL_GC = 1, CTRL_PAIR = 2, CTRL_JC_L = 3, CTRL_JC_R = 4;
export const isJoyCon = (t: number) => t >= CTRL_PAIR;

// USB modes (usb_mode_t).
export const MODE_SWITCH = 0, MODE_DS_EDGE = 1, MODE_DS = 2, MODE_X360 = 3, MODE_GC = 4, MODE_SINPUT = 5;
export const MODE_NAMES = [
  "Nintendo Switch Pro Controller",
  "DualSense Edge Wireless Controller",
  "DualSense Wireless Controller",
  "Xbox 360 Controller",
  "Nintendo GameCube Controller Adapter",
  "SInput Controller",
];
export const STATUS_MODES: Record<string, number> = {
  switch_pro: MODE_SWITCH, dualsense_edge: MODE_DS_EDGE, dualsense: MODE_DS, xbox360: MODE_X360, gc_adapter: MODE_GC, sinput: MODE_SINPUT,
};
// Mouse Mode: emulated Xbox 360 and SInput controllers only.
export const mouseMode = (mode: number) => mode === MODE_X360 || mode === MODE_SINPUT;

// Outputs the plugin itself carries out: the Quick Access menu
// (OUT_DECKY_QAM in Switch Pro mode, GP_DECKY_QAM in the others).
export const OUT_DECKY_QAM = 21, GP_DECKY_QAM = 28;
export const deckyQam = (mode: number) => (mode === MODE_SWITCH ? OUT_DECKY_QAM : GP_DECKY_QAM);

export const PIDS: Record<number, string> = {
  0x2069: "Nintendo Switch 2 Pro Controller",
  0x2073: "Nintendo GameCube Controller",
  0x2067: "Joy-Con 2 (L)",
  0x2066: "Joy-Con 2 (R)",
};

// Inputs (in_button_t).
export const IN_CAPTURE = 13, IN_GL = 18, IN_GR = 19, IN_C = 20, IN_SL_R = 21, IN_SR_R = 22;

// A controller's extra buttons: the input, its label, and which Joy-Con it is on.
export interface Extra { input: number; label: string; side?: string }
export function extraButtons(t: number): Extra[] {
  switch (t) {
    case CTRL_PRO:
      return [{ input: IN_GL, label: "GL" }, { input: IN_GR, label: "GR" }, { input: IN_C, label: "C" }, { input: IN_CAPTURE, label: "Capture" }];
    case CTRL_GC:
      return [{ input: IN_C, label: "C" }, { input: IN_CAPTURE, label: "Capture" }];
    case CTRL_PAIR:
      return [
        { input: IN_C, label: "C" }, { input: IN_CAPTURE, label: "Capture" },
        { input: IN_GL, label: "SL", side: "Joy-Con 2 (L)" }, { input: IN_GR, label: "SR", side: "Joy-Con 2 (L)" },
        { input: IN_SL_R, label: "SL", side: "Joy-Con 2 (R)" }, { input: IN_SR_R, label: "SR", side: "Joy-Con 2 (R)" },
      ];
    case CTRL_JC_L:
      return [{ input: IN_CAPTURE, label: "Capture" }, { input: IN_GL, label: "SL" }, { input: IN_GR, label: "SR" }];
    case CTRL_JC_R:
      return [{ input: IN_C, label: "C" }, { input: IN_GL, label: "SL" }, { input: IN_GR, label: "SR" }];
  }
  return [];
}

// Remapping from the controller (C + GL / GR, or SL / SR on a pair): the Pro
// Controller and a pair, except in DualSense Edge and SInput modes (the host
// remaps those).
export function quickRemap(t: number, mode: number): string | null {
  if (mode === MODE_DS_EDGE || mode === MODE_SINPUT) return null;
  if (t === CTRL_PRO) return "GL/GR";
  if (t === CTRL_PAIR) return "SL/SR";
  return null;
}

// Profile buttons (mode_slot_t order) for profiles on buttons.
export const SLOTS = ["A", "B", "X", "Y", "Up", "Down", "Left", "Right"];
export const slotLabel = (T: CtrlType, i: number) => (T.numbered ? String(i + 1) : SLOTS[i]);

// MOUSE_FLAG_* (settings.h)
export const MF_UP_DOWN_ONLY = 1, MF_INVERT_V = 2, MF_INVERT_H = 4;

// One profile as POST /api/settings takes it (see parse_profile()).
export function profileForm(t: number, i: number, P: Profile): string {
  const nums = [P.mode, P.deadzone, P.outer, P.swap, P.threshold, P.rumble, P.strength, ...P.map].join(",");
  const mouse = `m${P.mouse ? 1 : 0},${P.mouse_speed || 100},${P.mouse_flags & 7}`;
  return `prof_${t}_${i}=${nums},${mouse},${encodeURIComponent(P.name)}`;
}

// An output's name split into a title and, for shortcuts and the like
// ("Home+A (Steam quick access)"), a description line.
export function outputText(name: string): { title: string; desc: string } {
  const m = /^(.*) \((.*)\)$/.exec(name);
  if (m) return { title: m[1], desc: m[2].charAt(0).toUpperCase() + m[2].slice(1) };
  return { title: name, desc: "" };
}
