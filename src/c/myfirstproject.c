#include <pebble.h>
#include <string.h>
#include "message_keys.auto.h"

/*** Config ***/
#define MAX_SPRITES        8      // total flowers on screen (duplicates allowed)
#define LIGHT_OFF_MS       4000   // bloom duration before reshuffle

// layout (no runtime scaling; draw at native PNG size)
#define EDGE_BIAS_PCT      60     // % chance to place near screen edges
#define EDGE_MARGIN        12     // px band near edges

// repetition / spacing
#define NO_REPEAT_PAD      14     // avoid placing near last cycle’s spots
#define PLACE_ATTEMPTS     28     // tries per flower to find a good spot

// overlap control (allow some overlap, block heavy stacking)
#define MAX_OVERLAP_PCT    55     // reject if >55% of the smaller rect is covered
#define MIN_CENTER_DIST    6      // also keep centers at least 6 px apart

/*** Sprite type ***/
typedef struct {
  GBitmap *bmp_outline;
  GBitmap *bmp_fill;
  GRect    frame;      // position & native size
  int      flower_ix;  // which design (0..4)
  bool     filled;     // false=outline, true=filled
} Sprite;

/*** Globals ***/
static Window   *s_main_window;
static Layer    *s_canvas;
static TextLayer *s_date_layer, *s_time_layer, *s_info_layer;

static Sprite s_sprites[MAX_SPRITES];
static GRect  s_bounds;

static AppTimer *s_off_timer = NULL;

/*** Remember last positions so we don’t “repeat” ***/
static GRect s_prev_frames[MAX_SPRITES];
static int   s_prev_count = 0;

/*** Resource table (must match appinfo.json) ***/
typedef struct { uint32_t res_outline; uint32_t res_fill; } FlowerRes;
static const FlowerRes FLOWERS[] = {
  { RESOURCE_ID_FLOWER1_OUTLINE, RESOURCE_ID_FLOWER1_FILL },
  { RESOURCE_ID_FLOWER2_OUTLINE, RESOURCE_ID_FLOWER2_FILL },
  { RESOURCE_ID_FLOWER3_OUTLINE, RESOURCE_ID_FLOWER3_FILL },
  { RESOURCE_ID_FLOWER4_OUTLINE, RESOURCE_ID_FLOWER4_FILL },
  { RESOURCE_ID_FLOWER5_OUTLINE, RESOURCE_ID_FLOWER5_FILL }
};
static const int FLOWER_COUNT = (int)(sizeof(FLOWERS)/sizeof(FLOWERS[0]));

/*** Weather buffers ***/
static char s_temp_buf[8] = "--";
static char s_uvi_buf[8]  = "--";

/*** Rand ***/
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

static bool too_stacked(GRect a, GRect b){
  // heavy overlap test as % of the smaller area (integer math)
  int inter = rect_intersection_area(a, b);
  int area_a = a.size.w * a.size.h;
  int area_b = b.size.w * b.size.h;
  int area_min = (area_a < area_b) ? area_a : area_b;
  if(area_min > 0 && inter * 100 > area_min * MAX_OVERLAP_PCT) return true;

  // center separation guard
  int ax = a.origin.x + a.size.w/2, ay = a.origin.y + a.size.h/2;
  int bx = b.origin.x + b.size.w/2, by = b.origin.y + b.size.h/2;
  int dx = ax - bx, dy = ay - by;
  if(dx*dx + dy*dy < MIN_CENTER_DIST*MIN_CENTER_DIST) return true;

  return false;
}

static bool rects_intersect_padded(GRect a, GRect b, int pad){
  // used only for the "avoid last positions" check
  a.origin.x -= pad; a.origin.y -= pad; a.size.w += 2*pad; a.size.h += 2*pad;
  b.origin.x -= pad; b.origin.y -= pad; b.size.w += 2*pad; b.size.h += 2*pad;
  int ax2 = a.origin.x + a.size.w, ay2 = a.origin.y + a.size.h;
  int bx2 = b.origin.x + b.size.w, by2 = b.origin.y + b.size.h;
  if (ax2 <= b.origin.x) return false;
  if (bx2 <= a.origin.x) return false;
  if (ay2 <= b.origin.y) return false;
  if (by2 <= a.origin.y) return false;
  return true;
}

static bool near_any_previous(GRect r){
  for(int i=0;i<s_prev_count;i++){
    if(rects_intersect_padded(r, s_prev_frames[i], NO_REPEAT_PAD)) return true;
  }
  return false;
}

static void refresh_info_line(void) {
  static char line[24];
  snprintf(line, sizeof(line), "%s°  UV %s", s_temp_buf, s_uvi_buf);
  text_layer_set_text(s_info_layer, line);
}

static void free_sprites(void){
  for(int i=0;i<MAX_SPRITES;i++){
    if(s_sprites[i].bmp_outline){ gbitmap_destroy(s_sprites[i].bmp_outline); s_sprites[i].bmp_outline=NULL; }
    if(s_sprites[i].bmp_fill)   { gbitmap_destroy(s_sprites[i].bmp_fill);    s_sprites[i].bmp_fill=NULL; }
  }
}

/*** Placement (native bitmap size, no scaling/tiling) ***/
static GRect make_flower_rect(GBitmap *o){
  // native size
  GRect sb = gbitmap_get_bounds(o);
  const int fw = sb.size.w;
  const int fh = sb.size.h;

  // candidate position (edge-biased)
  int x=0, y=0;
  const bool edge = (rand_range(1,100) <= EDGE_BIAS_PCT);
  if(edge){
    switch(rand_range(0,3)){ // 0=top,1=bottom,2=left,3=right
      case 0: y = rand_range(0, EDGE_MARGIN);                                 x = rand_range(0, s_bounds.size.w - fw); break;
      case 1: y = rand_range(s_bounds.size.h - fh - EDGE_MARGIN, s_bounds.size.h - fh); x = rand_range(0, s_bounds.size.w - fw); break;
      case 2: x = rand_range(0, EDGE_MARGIN);                                 y = rand_range(0, s_bounds.size.h - fh); break;
      default:x = rand_range(s_bounds.size.w - fw - EDGE_MARGIN, s_bounds.size.w - fw); y = rand_range(0, s_bounds.size.h - fh); break;
    }
  } else {
    x = rand_range(0, s_bounds.size.w - fw);
    y = rand_range(0, s_bounds.size.h - fh);
  }

  return GRect(x, y, fw, fh);
}

static void place_one(int i, int flower_ix){
  // bitmaps
  GBitmap *o = gbitmap_create_with_resource(FLOWERS[flower_ix].res_outline);
  GBitmap *f = gbitmap_create_with_resource(FLOWERS[flower_ix].res_fill);

  // find a spot that isn't "stacked" and doesn't repeat last cycle
  GRect r;
  bool ok = false;
  for(int attempt=0; attempt<PLACE_ATTEMPTS && !ok; attempt++){
    r = make_flower_rect(o);
    ok = true;

    // allow light overlap, but avoid heavy stacking
    for(int k=0; k<i; k++){
      if(too_stacked(r, s_sprites[k].frame)) { ok = false; break; }
    }

    // avoid near-repeat of last positions
    if(ok && near_any_previous(r)) ok = false;
  }
  if(!ok){
    // last resort: just place anywhere
    r = make_flower_rect(o);
  }

  s_sprites[i].bmp_outline = o;
  s_sprites[i].bmp_fill    = f;
  s_sprites[i].frame       = r;
  s_sprites[i].flower_ix   = flower_ix;
  s_sprites[i].filled      = false;
}

static void generate_sprites(void){
  free_sprites();

  // guarantee at least one of each design (up to MAX_SPRITES)
  int i = 0;
  const int unique = (FLOWER_COUNT < MAX_SPRITES) ? FLOWER_COUNT : MAX_SPRITES;
  for(; i < unique; i++){
    place_one(i, i);
  }
  // fill remaining with random designs
  for(; i < MAX_SPRITES; i++){
    place_one(i, rand_range(0, FLOWER_COUNT - 1));
  }

  // remember positions for next cycle's "no repeat" rule
  s_prev_count = MAX_SPRITES;
  for(int j=0;j<MAX_SPRITES;j++) s_prev_frames[j] = s_sprites[j].frame;
}

/*** Drawing ***/
static void layer_update(Layer *layer, GContext *ctx){
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, s_bounds, 0, GCornerNone);

  // honor PNG transparency when bitmaps overlap
  graphics_context_set_compositing_mode(ctx, GCompOpSet);

  for(int i=0;i<MAX_SPRITES;i++){
    GBitmap *bmp = s_sprites[i].filled ? s_sprites[i].bmp_fill : s_sprites[i].bmp_outline;
    if(bmp) graphics_draw_bitmap_in_rect(ctx, bmp, s_sprites[i].frame);
  }
}

/*** Bloom / reshuffle ***/
static void light_off_cb(void *ctx){
  (void)ctx;
  generate_sprites();                         // new random positions
  for(int i=0;i<MAX_SPRITES;i++) s_sprites[i].filled = false;  // outlines
  layer_mark_dirty(s_canvas);
}

static void light_on_now(void){
  for(int i=0;i<MAX_SPRITES;i++) s_sprites[i].filled = true;   // bloom
  layer_mark_dirty(s_canvas);

  light_enable_interaction();
  if(s_off_timer) app_timer_cancel(s_off_timer);
  s_off_timer = app_timer_register(LIGHT_OFF_MS, light_off_cb, NULL);

  // optional: ask phone for weather refresh
  DictionaryIterator *out;
  if(app_message_outbox_begin(&out) == APP_MSG_OK) {
    app_message_outbox_send();
  }
}

/*** Input → trigger bloom ***/
static void tap_handler(AccelAxisType a, int32_t d){ (void)a;(void)d; light_on_now(); }
static void select_click_handler(ClickRecognizerRef r, void *c){ (void)r;(void)c; light_on_now(); }
static void up_click_handler(ClickRecognizerRef r, void *c){ (void)r;(void)c; light_on_now(); }
static void down_click_handler(ClickRecognizerRef r, void *c){ (void)r;(void)c; light_on_now(); }
static void click_config_provider(void *context){
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP,     up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN,   down_click_handler);
}

/*** Time / Date ***/
static void refresh_time_date(void){
  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  static char s_time_buf[8];
  if(clock_is_24h_style()){
    strftime(s_time_buf, sizeof(s_time_buf), "%H:%M", t);
  } else {
    strftime(s_time_buf, sizeof(s_time_buf), "%I:%M", t);
    if(s_time_buf[0]=='0') memmove(s_time_buf, s_time_buf+1, sizeof(s_time_buf)-1);
  }
  text_layer_set_text(s_time_layer, s_time_buf);

  static char s_date_buf[16];
  strftime(s_date_buf, sizeof(s_date_buf), "%a , %b %e", t);
  text_layer_set_text(s_date_layer, s_date_buf);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed){
  if(units_changed & MINUTE_UNIT) refresh_time_date();
}

/*** AppMessage (TEMP/UVI in) ***/
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

  // Flowers layer (behind text)
  s_canvas = layer_create(s_bounds);
  layer_set_update_proc(s_canvas, layer_update);
  layer_add_child(root, s_canvas);

  // Date ABOVE time
  s_date_layer = text_layer_create(GRect(0, s_bounds.size.h/2 - 56, s_bounds.size.w, 28));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorWhite);
  text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_date_layer));

  // Time center
  s_time_layer = text_layer_create(GRect(0, s_bounds.size.h/2 - 18, s_bounds.size.w, 44));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorWhite);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  // Info BELOW time
  s_info_layer = text_layer_create(GRect(0, s_bounds.size.h/2 + 32, s_bounds.size.w, 26));
  text_layer_set_background_color(s_info_layer, GColorClear);
  text_layer_set_text_color(s_info_layer, GColorWhite);
  text_layer_set_font(s_info_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_text_alignment(s_info_layer, GTextAlignmentCenter);
  text_layer_set_text(s_info_layer, "—°  UV —");
  layer_add_child(root, text_layer_get_layer(s_info_layer));

  // seed RNG with current time to increase variety across launches
  s_rand_state ^= (uint32_t)time(NULL);

  generate_sprites();
  layer_mark_dirty(s_canvas);
  refresh_time_date();
  refresh_info_line();

  // Show the effect immediately
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

static void init(void){
  s_main_window = window_create();
  window_set_background_color(s_main_window, GColorBlack);
  window_set_window_handlers(s_main_window, (WindowHandlers){
    .load = main_window_load,
    .unload = main_window_unload
  });
  window_stack_push(s_main_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(tap_handler);
  window_set_click_config_provider(s_main_window, click_config_provider);

  // AppMessage
  app_message_register_inbox_received(inbox_received_cb);
  app_message_register_inbox_dropped(inbox_dropped_cb);
  app_message_register_outbox_failed(outbox_failed_cb);
  app_message_register_outbox_sent(outbox_sent_cb);
  app_message_open(64, 16);
}

static void deinit(void){
  accel_tap_service_unsubscribe();
  window_destroy(s_main_window);
}

int main(void){ init(); app_event_loop(); deinit(); }
