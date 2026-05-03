#include <pebble.h>
#include <string.h>
#include "message_keys.auto.h"

/*** Protos ***/
static void generate_sprites(void);
static void light_off_cb(void *ctx);
static void light_on_now(void);
static void refresh_time_date(void);

/*** Config (targets) ***/
#define TARGET_SPRITES        8    // aim for 8
#define MIN_SPRITES           6    // never below 6
#define LIGHT_OFF_MS          4000

// layout & edges
#define EDGE_BIAS_START      10
#define EDGE_MARGIN          12
#define EDGE_MAX_START        2
#define OFFSCREEN_MARGIN     20    // allow partial off-screen (spill)

// repetition / spacing (tiered)
#define NO_REPEAT_START      16
#define PLACE_ATTEMPTS_1    180
#define PLACE_ATTEMPTS_2    260
#define PLACE_ATTEMPTS_3    360

// overlap/spacing (strict → relaxed)
#define MIN_DIST_TIER1       18
#define MIN_DIST_TIER2       16
#define MIN_DIST_TIER3       12
#define OVERLAP_PAD           1
#define TOUCH_PX_TIER1        0
#define TOUCH_PX_TIER2        0
#define TOUCH_PX_TIER3        4

// grid-sweep & spill-filler
#define GRID_STEP_1           6
#define GRID_STEP_2           5
#define GRID_STEP_3           4
#define SPILL_ATTEMPTS      240   // final filler to reach >=6 using off-screen

/*** Sprite ***/
typedef struct {
  GBitmap *bmp_outline;
  GBitmap *bmp_fill;
  GRect    frame;
  int      flower_ix;
  bool     filled;
} Sprite;

/*** Globals ***/
static Window   *s_main_window;
static Layer    *s_canvas;
static TextLayer *s_date_layer, *s_time_layer, *s_info_layer;

static Sprite s_sprites[TARGET_SPRITES];
static int    s_placed_count = 0;
static GRect  s_bounds;

static AppTimer *s_off_timer = NULL;

/*** “no-repeat” memory ***/
static GRect s_prev_frames[TARGET_SPRITES];
static int   s_prev_count = 0;

/*** Edge counters ***/
static int s_edge_top = 0, s_edge_bottom = 0, s_edge_left = 0, s_edge_right = 0;

/*** Resources ***/
typedef struct { uint32_t res_outline; uint32_t res_fill; } FlowerRes;
static const FlowerRes FLOWERS[] = {
#ifdef RESOURCE_ID_FLOWER1_OUTLINE
  { RESOURCE_ID_FLOWER1_OUTLINE, RESOURCE_ID_FLOWER1_FILL },
#endif
#ifdef RESOURCE_ID_FLOWER2_OUTLINE
  { RESOURCE_ID_FLOWER2_OUTLINE, RESOURCE_ID_FLOWER2_FILL },
#endif
#ifdef RESOURCE_ID_FLOWER3_OUTLINE
  { RESOURCE_ID_FLOWER3_OUTLINE, RESOURCE_ID_FLOWER3_FILL },
#endif
#ifdef RESOURCE_ID_FLOWER4_OUTLINE
  { RESOURCE_ID_FLOWER4_OUTLINE, RESOURCE_ID_FLOWER4_FILL },
#endif
#ifdef RESOURCE_ID_FLOWER5_OUTLINE
  { RESOURCE_ID_FLOWER5_OUTLINE, RESOURCE_ID_FLOWER5_FILL },
#endif
#ifdef RESOURCE_ID_FLOWER6_OUTLINE
  { RESOURCE_ID_FLOWER6_OUTLINE, RESOURCE_ID_FLOWER6_FILL },
#endif
#ifdef RESOURCE_ID_FLOWER7_OUTLINE
  { RESOURCE_ID_FLOWER7_OUTLINE, RESOURCE_ID_FLOWER7_FILL },
#endif
#ifdef RESOURCE_ID_FLOWER8_OUTLINE
  { RESOURCE_ID_FLOWER8_OUTLINE, RESOURCE_ID_FLOWER8_FILL },
#endif
#ifdef RESOURCE_ID_FLOWER9_OUTLINE
  { RESOURCE_ID_FLOWER9_OUTLINE, RESOURCE_ID_FLOWER9_FILL },
#endif
#ifdef RESOURCE_ID_FLOWER10_OUTLINE
  { RESOURCE_ID_FLOWER10_OUTLINE, RESOURCE_ID_FLOWER10_FILL },
#endif
};
static const int FLOWER_COUNT = (int)(sizeof(FLOWERS)/sizeof(FLOWERS[0]));

/*** Weather ***/
static char s_temp_buf[8] = "--";
static char s_uvi_buf[8]  = "--";

/*** RNG ***/
static uint32_t s_rand_state = 0xA341316C;
static uint32_t lcg(void){ s_rand_state = 1664525*s_rand_state + 1013904223; return s_rand_state; }
static int rand_range(int lo, int hi){
  if(hi < lo) return lo;
  return lo + (int)(lcg() % (uint32_t)(hi - lo + 1));
}

/*** Helpers ***/
static int rect_intersection_area(GRect a, GRect b){
  int ax2 = a.origin.x + a.size.w, ay2 = a.origin.y + a.size.h;
  int bx2 = b.origin.x + b.size.w, by2 = b.origin.y + b.size.h;
  int ix = (ax2 < bx2 ? ax2 : bx2) - (a.origin.x > b.origin.x ? a.origin.x : b.origin.x);
  int iy = (ay2 < by2 ? ay2 : by2) - (a.origin.y > b.origin.y ? a.origin.y : b.origin.y);
  if(ix <= 0 || iy <= 0) return 0;
  return ix * iy;
}
static GRect inflate_rect(GRect r, int pad){
  r.origin.x -= pad; r.origin.y -= pad;
  r.size.w   += 2*pad; r.size.h += 2*pad;
  return r;
}
static bool rects_intersect_padded(GRect a, GRect b, int pad){
  a = inflate_rect(a, pad); b = inflate_rect(b, pad);
  int ax2 = a.origin.x + a.size.w, ay2 = a.origin.y + a.size.h;
  int bx2 = b.origin.x + b.size.w, by2 = b.origin.y + b.size.h;
  if (ax2 <= b.origin.x) return false;
  if (bx2 <= a.origin.x) return false;
  if (ay2 <= b.origin.y) return false;
  if (by2 <= a.origin.y) return false;
  return true;
}

/*** Info line ***/
static void refresh_info_line(void) {
  static char line[24];
  snprintf(line, sizeof(line), "%s°, UV %s", s_temp_buf, s_uvi_buf);
  text_layer_set_text(s_info_layer, line);
}

/*** Free memory ***/
static void free_sprites(void){
  for(int i=0;i<TARGET_SPRITES;i++){
    if(s_sprites[i].bmp_outline){ gbitmap_destroy(s_sprites[i].bmp_outline); s_sprites[i].bmp_outline=NULL; }
    if(s_sprites[i].bmp_fill)   { gbitmap_destroy(s_sprites[i].bmp_fill);    s_sprites[i].bmp_fill=NULL; }
  }
}

/*** Edge tracking ***/
static bool near_edge(GRect r, int margin, int which){
  switch(which){
    case 0: return r.origin.y <= margin;
    case 1: return r.origin.y + r.size.h >= s_bounds.size.h - margin;
    case 2: return r.origin.x <= margin;
    default:return r.origin.x + r.size.w >= s_bounds.size.w - margin;
  }
}

/*** Candidate generation (allows off-screen spill) ***/
static GRect make_flower_rect(GBitmap *o, int edge_bias_pct){
  GRect sb = gbitmap_get_bounds(o);
  const int fw = sb.size.w, fh = sb.size.h;

  // Allow positions slightly outside view; gfx will clip automatically
  const int min_x = -OFFSCREEN_MARGIN;
  const int max_x = s_bounds.size.w - fw + OFFSCREEN_MARGIN;
  const int min_y = -OFFSCREEN_MARGIN;
  const int max_y = s_bounds.size.h - fh + OFFSCREEN_MARGIN;

  int x=0, y=0;
  const bool edge_choice = (rand_range(1,100) <= edge_bias_pct);
  if(edge_choice){
    switch(rand_range(0,3)){
      case 0: y = rand_range(min_y, 0 + EDGE_MARGIN);                               x = rand_range(min_x, max_x); break;
      case 1: y = rand_range(s_bounds.size.h - fh - EDGE_MARGIN, max_y);            x = rand_range(min_x, max_x); break;
      case 2: x = rand_range(min_x, 0 + EDGE_MARGIN);                               y = rand_range(min_y, max_y); break;
      default:x = rand_range(s_bounds.size.w - fw - EDGE_MARGIN, max_x);            y = rand_range(min_y, max_y); break;
    }
  } else {
    x = rand_range(min_x, max_x);
    y = rand_range(min_y, max_y);
  }
  return GRect(x, y, fw, fh);
}

/*** Overlap rule (tiered) ***/
static bool too_stacked(GRect a, GRect b, int min_center_dist, int touch_px){
  const int ax = a.origin.x + a.size.w/2, ay = a.origin.y + a.size.h/2;
  const int bx = b.origin.x + b.size.w/2, by = b.origin.y + b.size.h/2;
  const int dx = ax - bx, dy = ay - by;
  if(dx*dx + dy*dy < min_center_dist*min_center_dist) return true;

  a = inflate_rect(a, OVERLAP_PAD);
  b = inflate_rect(b, OVERLAP_PAD);
  int inter = rect_intersection_area(a, b);
  if(inter == 0) return false;
  return !(inter <= touch_px); // allow only tiny “kisses” if touch_px>0
}

/*** Tiered placer (random + grid sweep) ***/
typedef struct {
  int edge_bias_pct;
  int edge_max;
  int no_repeat_pad;
  int min_center_dist;
  int touch_px;
  int place_attempts;
  int grid_step;
  bool ignore_no_repeat;
  bool ignore_edge_caps;
} PlaceParams;

static bool try_place_one(int placed_so_far, int flower_ix, GRect *out_frame, PlaceParams P){
  GBitmap *o = gbitmap_create_with_resource(FLOWERS[flower_ix].res_outline);
  if(!o) return false;

  // Phase 1: randomized attempts (with spill)
  for(int attempt=0; attempt<P.place_attempts; attempt++){
    GRect r = make_flower_rect(o, P.edge_bias_pct);

    int edge_hit = -1;
    if(near_edge(r, EDGE_MARGIN, 0)) edge_hit = 0;
    else if(near_edge(r, EDGE_MARGIN, 1)) edge_hit = 1;
    else if(near_edge(r, EDGE_MARGIN, 2)) edge_hit = 2;
    else if(near_edge(r, EDGE_MARGIN, 3)) edge_hit = 3;

    if(!P.ignore_edge_caps){
      if(edge_hit == 0 && s_edge_top    >= P.edge_max)   continue;
      if(edge_hit == 1 && s_edge_bottom >= P.edge_max)   continue;
      if(edge_hit == 2 && s_edge_left   >= P.edge_max)   continue;
      if(edge_hit == 3 && s_edge_right  >= P.edge_max)   continue;
    }

    bool bad = false;
    for(int k=0; k<placed_so_far; k++){
      if(too_stacked(r, s_sprites[k].frame, P.min_center_dist, P.touch_px)){ bad = true; break; }
    }
    if(bad) continue;

    if(!P.ignore_no_repeat){
      for(int i=0;i<s_prev_count;i++){
        if(rects_intersect_padded(r, s_prev_frames[i], P.no_repeat_pad)){ bad = true; break; }
      }
      if(bad) continue;
    }

    *out_frame = r;
    if(edge_hit == 0) s_edge_top++;
    else if(edge_hit == 1) s_edge_bottom++;
    else if(edge_hit == 2) s_edge_left++;
    else if(edge_hit == 3) s_edge_right++;
    gbitmap_destroy(o);
    return true;
  }

  // Phase 2: deterministic grid sweep (viewport only)
  GRect sb = gbitmap_get_bounds(o);
  const int fw = sb.size.w, fh = sb.size.h;
  for(int y = 0; y <= s_bounds.size.h - fh; y += P.grid_step){
    for(int x = 0; x <= s_bounds.size.w - fw; x += P.grid_step){
      GRect r = GRect(x, y, fw, fh);

      int edge_hit = -1;
      if(near_edge(r, EDGE_MARGIN, 0)) edge_hit = 0;
      else if(near_edge(r, EDGE_MARGIN, 1)) edge_hit = 1;
      else if(near_edge(r, EDGE_MARGIN, 2)) edge_hit = 2;
      else if(near_edge(r, EDGE_MARGIN, 3)) edge_hit = 3;

      if(!P.ignore_edge_caps){
        if(edge_hit == 0 && s_edge_top    >= P.edge_max)   continue;
        if(edge_hit == 1 && s_edge_bottom >= P.edge_max)   continue;
        if(edge_hit == 2 && s_edge_left   >= P.edge_max)   continue;
        if(edge_hit == 3 && s_edge_right  >= P.edge_max)   continue;
      }

      bool bad = false;
      for(int k=0; k<placed_so_far; k++){
        if(too_stacked(r, s_sprites[k].frame, P.min_center_dist, P.touch_px)){ bad = true; break; }
      }
      if(bad) continue;

      if(!P.ignore_no_repeat){
        for(int i=0;i<s_prev_count;i++){
          if(rects_intersect_padded(r, s_prev_frames[i], P.no_repeat_pad)){ bad = true; break; }
        }
        if(bad) continue;
      }

      *out_frame = r;
      if(edge_hit == 0) s_edge_top++;
      else if(edge_hit == 1) s_edge_bottom++;
      else if(edge_hit == 2) s_edge_left++;
      else if(edge_hit == 3) s_edge_right++;
      gbitmap_destroy(o);
      return true;
    }
  }

  gbitmap_destroy(o);
  return false;
}

static void place_commit(int i, int flower_ix, GRect frame){
  s_sprites[i].bmp_outline = gbitmap_create_with_resource(FLOWERS[flower_ix].res_outline);
  s_sprites[i].bmp_fill    = gbitmap_create_with_resource(FLOWERS[flower_ix].res_fill);
  s_sprites[i].frame       = frame;
  s_sprites[i].flower_ix   = flower_ix;
  s_sprites[i].filled      = false;
}

/*** Final filler: aggressively spill off-screen until >=6 ***/
static void spill_fill_to_min(const int idxs[], int cap, int min_needed){
  for(int i = s_placed_count; i < min_needed && i < TARGET_SPRITES; i++){
    int flower_ix = idxs[i % cap];
    GBitmap *o = gbitmap_create_with_resource(FLOWERS[flower_ix].res_outline);
    if(!o) continue;
    GRect sb = gbitmap_get_bounds(o);
    const int fw = sb.size.w, fh = sb.size.h;

    const int min_x = -OFFSCREEN_MARGIN - fw/3;
    const int max_x = s_bounds.size.w - fw + OFFSCREEN_MARGIN + fw/3;
    const int min_y = -OFFSCREEN_MARGIN - fh/3;
    const int max_y = s_bounds.size.h - fh + OFFSCREEN_MARGIN + fh/3;

    bool placed = false;
    for(int attempts=0; attempts<SPILL_ATTEMPTS && !placed; attempts++){
      GRect r = GRect(rand_range(min_x, max_x), rand_range(min_y, max_y), fw, fh);
      bool bad = false;
      for(int k=0; k<s_placed_count; k++){
        if(too_stacked(r, s_sprites[k].frame, MIN_DIST_TIER3, TOUCH_PX_TIER3)){ bad = true; break; }
      }
      if(!bad){
        place_commit(s_placed_count, flower_ix, r);
        s_placed_count++;
        placed = true;
      }
    }
    gbitmap_destroy(o);
    if(!placed) break; // unlikely
  }
}

/*** Generation with hard min and spill ***/
static void generate_sprites(void){
  free_sprites();
  s_edge_top = s_edge_bottom = s_edge_left = s_edge_right = 0;
  s_placed_count = 0;

  if(FLOWER_COUNT == 0){ s_prev_count = 0; return; }

  // shuffle indices
  int idxs[32];
  const int cap = (FLOWER_COUNT > 32) ? 32 : FLOWER_COUNT;
  for(int n=0;n<cap;n++) idxs[n] = n;
  for(int n=cap-1;n>0;n--){ int j = (int)(lcg() % (uint32_t)(n+1)); int t = idxs[n]; idxs[n]=idxs[j]; idxs[j]=t; }

  typedef struct {
    int edge_bias_pct, edge_max, no_repeat_pad, min_center_dist, touch_px, place_attempts, grid_step;
    bool ignore_no_repeat, ignore_edge_caps;
  } P;

  P T1 = { EDGE_BIAS_START, EDGE_MAX_START, NO_REPEAT_START, MIN_DIST_TIER1, TOUCH_PX_TIER1, PLACE_ATTEMPTS_1, GRID_STEP_1, false, false };
  P T2 = { EDGE_BIAS_START, EDGE_MAX_START, NO_REPEAT_START, MIN_DIST_TIER2, TOUCH_PX_TIER2, PLACE_ATTEMPTS_2, GRID_STEP_2, false, false };
  P T3 = { EDGE_BIAS_START, EDGE_MAX_START, NO_REPEAT_START, MIN_DIST_TIER3, TOUCH_PX_TIER3, PLACE_ATTEMPTS_3, GRID_STEP_3, true,  true  };

  const P TIERS[3] = { T1, T2, T3 };

  for(int tier=0; tier<3; tier++){
    s_edge_top=s_edge_bottom=s_edge_left=s_edge_right=0;
    s_placed_count = 0;

    for(int i=0; i<TARGET_SPRITES; i++){
      int flower_ix = idxs[i % cap];
      GRect frame;
      PlaceParams PP = { TIERS[tier].edge_bias_pct, TIERS[tier].edge_max, TIERS[tier].no_repeat_pad,
                         TIERS[tier].min_center_dist, TIERS[tier].touch_px, TIERS[tier].place_attempts,
                         TIERS[tier].grid_step, TIERS[tier].ignore_no_repeat, TIERS[tier].ignore_edge_caps };
      if(!try_place_one(s_placed_count, flower_ix, &frame, PP)) break;
      place_commit(s_placed_count, flower_ix, frame);
      s_placed_count++;
    }

    if(s_placed_count >= TARGET_SPRITES || s_placed_count >= MIN_SPRITES){
      break;
    }
  }

  if(s_placed_count < MIN_SPRITES){
    spill_fill_to_min(idxs, cap, MIN_SPRITES);
  }

  s_prev_count = s_placed_count;
  for(int j=0;j<s_placed_count && j<TARGET_SPRITES;j++) s_prev_frames[j] = s_sprites[j].frame;
}

/*** Drawing ***/
static void layer_update(Layer *layer, GContext *ctx){
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, s_bounds, 0, GCornerNone);
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  for(int i=0;i<s_placed_count;i++){
    GBitmap *bmp = s_sprites[i].filled ? s_sprites[i].bmp_fill : s_sprites[i].bmp_outline;
    if(bmp) graphics_draw_bitmap_in_rect(ctx, bmp, s_sprites[i].frame);
  }
}

/*** Bloom / reshuffle ***/
static void light_off_cb(void *ctx){
  (void)ctx;
  generate_sprites();
  for(int i=0;i<s_placed_count;i++) s_sprites[i].filled = false;
  layer_mark_dirty(s_canvas);
}
static void light_on_now(void){
  for(int i=0;i<s_placed_count;i++) s_sprites[i].filled = true;
  layer_mark_dirty(s_canvas);
  light_enable_interaction();
  if(s_off_timer) app_timer_cancel(s_off_timer);
  s_off_timer = app_timer_register(LIGHT_OFF_MS, light_off_cb, NULL);
  DictionaryIterator *out;
  if(app_message_outbox_begin(&out) == APP_MSG_OK) { app_message_outbox_send(); }
}

/*** Input (watchface-safe) ***/
// Wrist flick / tap → bloom
static void tap_handler(AccelAxisType a, int32_t d){ (void)a;(void)d; light_on_now(); }

// Regain attention (focus) → bloom
static void focus_handler(bool in_focus){
  if(in_focus) light_on_now();
}
/*** Time / Date ***/
static void refresh_time_date(void){
  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  static char s_time_buf[8];

  if (clock_is_24h_style()) {
    // 24h, space-padded hour (e.g., " 9:05")
    strftime(s_time_buf, sizeof(s_time_buf), "%k:%M", t);
  } else {
    // 12h, space-padded hour (e.g., " 9:05")
    strftime(s_time_buf, sizeof(s_time_buf), "%l:%M", t);
  }
  // Trim leading space so " 9:05" -> "9:05"
  if (s_time_buf[0] == ' ') {
    memmove(s_time_buf, s_time_buf + 1, strlen(s_time_buf));
  }

  text_layer_set_text(s_time_layer, s_time_buf);

  static char s_date_prefix[12];
  static char s_date_buf[16];
  strftime(s_date_prefix, sizeof(s_date_prefix), "%a, %b", t);
  snprintf(s_date_buf, sizeof(s_date_buf), "%s %d", s_date_prefix, t->tm_mday);
  text_layer_set_text(s_date_layer, s_date_buf);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed){
  if (units_changed & MINUTE_UNIT) refresh_time_date();

}

/*** AppMessage ***/
static void inbox_received_cb(DictionaryIterator *iter, void *ctx) {
  Tuple *t_temp = dict_find(iter, MESSAGE_KEY_TEMP);
  Tuple *t_uvi  = dict_find(iter, MESSAGE_KEY_UVI);
  if(t_temp) snprintf(s_temp_buf, sizeof(s_temp_buf), "%ld", t_temp->value->int32);
  if(t_uvi)  snprintf(s_uvi_buf,  sizeof(s_uvi_buf),  "%ld", t_uvi->value->int32);
  refresh_info_line();
}
static void inbox_dropped_cb(AppMessageResult reason, void *ctx) { (void)reason;(void)ctx; }
static void outbox_failed_cb(DictionaryIterator *iter, AppMessageResult reason, void *ctx) { (void)iter;(void)reason;(void)ctx; }
static void outbox_sent_cb(DictionaryIterator *iter, void *ctx) { (void)iter;(void)ctx; }

/*** Window lifecycle ***/
static void main_window_load(Window *window){
  Layer *root = window_get_root_layer(window);
  s_bounds = layer_get_bounds(root);

  s_canvas = layer_create(s_bounds);
  layer_set_update_proc(s_canvas, layer_update);
  layer_add_child(root, s_canvas);

  const bool compact_layout = (s_bounds.size.w <= 144 && s_bounds.size.h <= 168); // Pebble Quick View-safe
  const int text_center_y = s_bounds.size.h / 2;
  const int date_h = 30;
  const int time_h = 62;
  const int info_h = 34;
  const int gap_date_time = compact_layout ? 7 : 10;
  const int gap_time_info = compact_layout ? 4 : 6;
  const int text_y_offset = compact_layout ? -12 : 0;

  const int date_y = text_center_y - (date_h + gap_date_time + time_h + gap_time_info + info_h) / 2 + text_y_offset;
  const int time_y = date_y + date_h + gap_date_time;
  const int info_y = time_y + time_h + gap_time_info;

  s_date_layer = text_layer_create(GRect(0, date_y, s_bounds.size.w, date_h));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorWhite);
  text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_date_layer));

  s_time_layer = text_layer_create(GRect(0, time_y, s_bounds.size.w, time_h));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorWhite);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  s_info_layer = text_layer_create(GRect(0, info_y, s_bounds.size.w, info_h));
  text_layer_set_background_color(s_info_layer, GColorClear);
  text_layer_set_text_color(s_info_layer, GColorWhite);
  text_layer_set_font(s_info_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_info_layer, GTextAlignmentCenter);
  text_layer_set_text(s_info_layer, "—°  UV —");
  layer_add_child(root, text_layer_get_layer(s_info_layer));

  s_rand_state ^= (uint32_t)time(NULL);

  generate_sprites();
  layer_mark_dirty(s_canvas);
  refresh_time_date();
  refresh_info_line();
  light_on_now();
}

static void main_window_unload(Window *window){
  if(s_off_timer) app_timer_cancel(s_off_timer);
  text_layer_destroy(s_info_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_date_layer);
  layer_destroy(s_canvas);
  free_sprites();
}

/*** App init/deinit ***/
static void init(void){
  s_main_window = window_create();
  window_set_background_color(s_main_window, GColorBlack);
  window_set_window_handlers(s_main_window, (WindowHandlers){ .load = main_window_load, .unload = main_window_unload });
  window_stack_push(s_main_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(tap_handler);

  // Bloom automatically when regaining attention
  app_focus_service_subscribe_handlers((AppFocusHandlers){
    .did_focus = focus_handler
  });

  app_message_register_inbox_received(inbox_received_cb);
  app_message_register_inbox_dropped(inbox_dropped_cb);
  app_message_register_outbox_failed(outbox_failed_cb);
  app_message_register_outbox_sent(outbox_sent_cb);
  app_message_open(64, 16);
}

static void deinit(void){
  app_focus_service_unsubscribe();
  accel_tap_service_unsubscribe();
  window_destroy(s_main_window);
}

int main(void){ init(); app_event_loop(); deinit(); }
