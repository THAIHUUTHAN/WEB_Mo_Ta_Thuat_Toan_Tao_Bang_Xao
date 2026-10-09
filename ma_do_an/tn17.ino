/* =====================================================================
 * TN17 — FIRMWARE BỀN VỮNG: ESP32-CAM -> CPE -> MQTT  (bản v2 của TN16)
 * Đồ án: Thái Hữu Thân — AT19A — AT190149 — GVHD: ThS. Hoàng Thu Phương
 * ---------------------------------------------------------------------
 * TN17 KHÔNG đổi thuật toán. Vùng CORE dưới đây giữ NGUYÊN VĂN của TN16
 * để mọi vân tay vẫn so sánh được. TN17 chỉ sửa phần VẬN HÀNH, nhằm trả
 * lời BỐN câu hỏi mà log TN16 KHÔNG trả lời được:
 *
 *   C1. Lần đo TN15/TN16 chạy ở xung nhịp CPU nào?  (cảnh báo của bản V2.1:
 *       một lần đo lặp cho số chậm hơn 1,45 lần -> nghi lần đầu không ở
 *       160 MHz.)  -> TN17 IN XUNG NHỊP, kèm nguồn XTAL và bản SDK.
 *   C2. Bộ đếm counter_n có thật sự KHÔNG LẶP qua reboot không?
 *       TN16 chạy 12 phút ở 1 kh/giây = 720 khung < CNT_STRIDE = 1000, nên
 *       trong suốt lần chạy KHÔNG có lấy một lần ghi NVS nào. Log vì thế
 *       không chứng minh được gì.  -> TN17 có CHE_DO 2 tự khởi động lại
 *       nhiều lần và in đủ dữ liệu để script kiểm tra tính không chồng lấn.
 *   C3. Có rò rỉ bộ nhớ không? TN16 chỉ in heap THẤP NHẤT ở phần tổng kết
 *       — một con số duy nhất thì không dựng được đường theo thời gian và
 *       không phân biệt được rò rỉ với phân mảnh.  -> TN17 in dòng HEAP
 *       định kỳ, có cả KHỐI TRỐNG LỚN NHẤT (phát hiện phân mảnh).
 *   C4. Mất WiFi/MQTT thì hệ mất bao lâu để trở lại? TN16 gọi wifi_ensure()
 *       CHẶN tới 15 giây ngay trong loop(), phá nhịp 1 kh/giây và làm hỏng
 *       chính phép đo fps.  -> TN17 kết nối lại KHÔNG CHẶN, có backoff, và
 *       cộng dồn THỜI GIAN MẤT KẾT NỐI.
 *
 * CẤU HÌNH ĐÃ CHỐT (TN15): XCLK 20 MHz, fb_count = 2, khung trong PSRAM,
 *                          QVGA 320x240 GRAYSCALE, JPEG Q = 20, PP-C.
 * BẮT BUỘC khi biên dịch: -ffp-contract=off (giữ để đối chiếu với TN12..TN16).
 * ===================================================================== */

#include "esp_camera.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "mbedtls/md.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "soc/rtc.h"

/* ------------------------ 1. THAM SỐ PHẢI SỬA ------------------------ */
static const char *WIFI_SSID = "DOI_TEN_WIFI";
static const char *WIFI_PASS = "DOI_MAT_KHAU";
static const char *MQTT_HOST = "192.168.1.100";   // IP máy chạy mosquitto
static const uint16_t MQTT_PORT = 1883;
static const char *DEV_ID = "esp01";

/* ------------------------ 2. CHỌN CHẾ ĐỘ ----------------------------- */
/*  CHE_DO 1 = CHẠY DÀI THẬT   : camera + MQTT, RUN_MINUTES phút, đóng ngưỡng B.
 *  CHE_DO 2 = KIỂM BẪY R3     : KHÔNG cần camera, KHÔNG cần WiFi. Quay vòng
 *             bộ đếm thật nhanh, tự esp_restart() SO_LAN_RESET lần, in đủ
 *             mốc để tn17_phan_tich.py chứng minh các khoảng counter của các
 *             lần khởi động KHÔNG chồng lấn nhau.
 *  CHE_DO 3 = ĐO XUNG NHỊP + VÂN TAY: không camera, không WiFi; in xung nhịp,
 *             chạy lại phép đo thuần CPU của TN15 ở 80/160/240 MHz. (Nếu chỉ
 *             cần việc này thì nạp tn17b_xungnhip.ino cho nhẹ.)          */
#define CHE_DO          1

/* ------------------------ 3. THAM SỐ THÍ NGHIỆM ---------------------- */
#define XCLK_HZ         20000000
#define JPEG_Q          20
#define FPS_PERIOD_MS   1000        // điểm vận hành 1,0 khung/giây
#define RUN_MINUTES     12          // >= 10 phút, dư 2 phút
#define PERM_MODE       2           // 0 = không hoán vị, 2 = PP-C
#define ECHO_ENABLE     1
#define HEAP_LOG_S      10          // chu kỳ ghi nhật ký bộ nhớ (giây)
#define CPU_MHZ_MONG_MUON 240       // 0 = không ép; đặt 240 cho cấu hình chốt

/* --- bẫy R3 --- */
#define CNT_STRIDE      1000        // bước dự trữ NVS ở CHẠY THẬT
#define CNT_STRIDE_R3   16          // bước NHỎ dùng cho CHE_DO 2 để kiểm được
#define SO_LAN_RESET    5           // CHE_DO 2 tự khởi động lại mấy lần
#define KHUNG_MOI_LAN   40          // CHE_DO 2: mỗi lần chạy tiêu thụ bấy nhiêu counter

/* K_master THÍ NGHIỆM — 00 01 02 ... 1f. Hệ thật phải nạp khóa riêng. */
static const uint8_t K_MASTER[32] = {
  0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
  0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f };

#define W_IMG 320
#define H_IMG 240
#define BS    8
#define NBLK  1200
#define FRAME_BYTES (W_IMG*H_IMG)

/* ------------------------ 4. CHÂN CAMERA AI THINKER ------------------ */
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

/* ===================================================================== */
/* ===CORE_BEGIN===  GIỮ NGUYÊN VĂN TỪ TN16. Script tn17_core_check.py   */
/*                   cắt đúng vùng này ra tệp C và biên dịch trên PC.    */
/*                   KHÔNG được dùng Arduino API trong vùng này.         */
/* ===================================================================== */

/* ---- CRC-16/CCITT-FALSE, bản TRA BẢNG (nhanh 7,9 lần bản từng bit) --- */
static uint16_t g_crc_tab[256];
static void crc16_init_table(void) {
  for (int i = 0; i < 256; i++) {
    uint16_t c = (uint16_t)(i << 8);
    for (int b = 0; b < 8; b++)
      c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    g_crc_tab[i] = c;
  }
}
static uint16_t crc16_ccitt_false(const uint8_t *d, uint32_t n) {
  uint16_t crc = 0xFFFF;
  for (uint32_t i = 0; i < n; i++)
    crc = (uint16_t)((crc << 8) ^ g_crc_tab[((crc >> 8) ^ d[i]) & 0xFF]);
  return crc;
}

/* ---- xoshiro128** (Q7) — CHỈ số nguyên 32 bit, không lệch nền tảng --- */
typedef struct { uint32_t s[4]; } xoshiro_t;
static inline uint32_t rotl32(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
static uint32_t xoshiro_next(xoshiro_t *g) {
  uint32_t *s = g->s;
  uint32_t out = rotl32(s[1] * 5u, 7) * 9u;
  uint32_t t = s[1] << 9;
  s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t;
  s[3] = rotl32(s[3], 11);
  return out;
}
/* Q7a: 128 bit đầu của thẻ HMAC, mỗi 4 byte đọc BIG-ENDIAN */
static void xoshiro_seed_from_tag(xoshiro_t *g, const uint8_t *T) {
  for (int k = 0; k < 4; k++)
    g->s[k] = ((uint32_t)T[4*k] << 24) | ((uint32_t)T[4*k+1] << 16) |
              ((uint32_t)T[4*k+2] << 8) | (uint32_t)T[4*k+3];
}
/* Q7b: Fisher-Yates giảm dần + Lemire bounded (KHÔNG chia dư) */
static void perm_C_from_tag(const uint8_t *T, uint16_t *p, int n) {
  xoshiro_t g; xoshiro_seed_from_tag(&g, T);
  for (int i = 0; i < n; i++) p[i] = (uint16_t)i;
  for (int i = n - 1; i > 0; i--) {
    uint32_t j = (uint32_t)(((uint64_t)xoshiro_next(&g) * (uint64_t)(i + 1)) >> 32);
    uint16_t t = p[i]; p[i] = p[j]; p[j] = t;
  }
}

/* ---- Áp hoán vị khối: out[khối i] = in[khối perm[i]] ----------------
 * Cách duyệt THEO KHỐI (TN15: nhanh hơn theo hàng 0,8..4,1 %).        */
static void permute_blocks(const uint8_t *in, uint8_t *out, const uint16_t *perm) {
  const int bw = W_IMG / BS;
  for (int i = 0; i < NBLK; i++) {
    int src = perm[i];
    int di = i / bw, dj = i - di * bw;
    int si = src / bw, sj = src - si * bw;
    const uint8_t *sp = in  + (si * BS) * W_IMG + sj * BS;
    uint8_t       *dp = out + (di * BS) * W_IMG + dj * BS;
    for (int row = 0; row < BS; row++) {
      for (int c = 0; c < BS; c++) dp[c] = sp[c];
      sp += W_IMG; dp += W_IMG;
    }
  }
}

/* ---- Header bản tin CPE 12 byte big-endian (đã chốt ở TN5) ----------
 * MAGIC 0x43 | VER 0x01 | FLAGS | r | counter(4) | LEN(2) | CRC(2)    */
static void build_header(uint8_t *h, uint8_t flags, uint8_t r,
                         uint32_t counter_n, uint16_t len, uint16_t crc) {
  h[0]=0x43; h[1]=0x01; h[2]=flags; h[3]=r;
  h[4]=(uint8_t)(counter_n>>24); h[5]=(uint8_t)(counter_n>>16);
  h[6]=(uint8_t)(counter_n>>8);  h[7]=(uint8_t)(counter_n);
  h[8]=(uint8_t)(len>>8);  h[9]=(uint8_t)(len);
  h[10]=(uint8_t)(crc>>8); h[11]=(uint8_t)(crc);
}

/* ---- FNV-1a 64 bit — chỉ dùng làm VÂN TAY đối chiếu PC <-> ESP32 ---- */
static uint64_t fnv1a64(const uint8_t *b, uint32_t n) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (uint32_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
  return h;
}
static uint64_t perm_fnv(const uint16_t *p, int n) {
  /* QUY ƯỚC: uint16 LITTLE-ENDIAN rồi FNV-1a */
  uint64_t h = 0xcbf29ce484222325ULL;
  for (int i = 0; i < n; i++) {
    h ^= (uint8_t)(p[i] & 0xFF);   h *= 0x100000001b3ULL;
    h ^= (uint8_t)(p[i] >> 8);     h *= 0x100000001b3ULL;
  }
  return h;
}

/* ---- Ảnh mẫu tất định dùng chung cho mọi phép đối chiếu ------------- */
static void xorshift32_image(uint8_t *out, uint32_t n, uint32_t seed) {
  uint32_t x = seed;
  for (uint32_t i = 0; i < n; i++) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    out[i] = (uint8_t)(x >> 24);
  }
}
/* ===CORE_END=== */
/* ===================================================================== */

/* ------------------------ 5. BIẾN TOÀN CỤC --------------------------- */
WiFiClient    g_wifi;
PubSubClient  g_mqtt(g_wifi);
Preferences   g_nvs;

static uint16_t *g_perm   = NULL;     // 1200 * 2 B, RAM nội
static uint8_t  *g_permed = NULL;     // 76800 B, PSRAM
static uint8_t  *g_jbuf   = NULL;     // bộ đệm gom JPEG 32 KiB, PSRAM
static volatile uint32_t g_jlen = 0;
static uint32_t g_counter = 0;
static uint32_t g_counter_dau = 0;    // counter của khung ĐẦU TIÊN lần khởi động này
static uint32_t g_boot_id = 0;
static uint32_t g_stride = CNT_STRIDE;

char g_topic_frame[48], g_topic_stat[48], g_topic_lwt[48];
char g_topic_echo[48],  g_topic_echoback[48];

/* --- thống kê chạy dài --- */
static uint32_t st_sent = 0, st_fail_cap = 0, st_fail_jpg = 0, st_fail_pub = 0;
static uint32_t st_wifi_reconn = 0, st_mqtt_reconn = 0;
static uint32_t st_wifi_down_ms = 0, st_mqtt_down_ms = 0;
static uint32_t st_wifi_down_max = 0, st_mqtt_down_max = 0;
static uint32_t st_heap_min = 0xFFFFFFFF, st_psram_min = 0xFFFFFFFF;
static uint32_t st_nvs_writes = 0;
static uint32_t st_tre_nhip = 0;

/* --- đo độ trễ khứ hồi: vòng đệm 64 khung gần nhất --- */
#define ECHO_SLOTS 64
static uint32_t ec_cnt[ECHO_SLOTS];
static uint32_t ec_t0[ECHO_SLOTS];
static uint8_t  ec_used[ECHO_SLOTS];
static uint32_t st_rtt_n = 0, st_rtt_sum = 0, st_rtt_min = 0xFFFFFFFF, st_rtt_max = 0;
static uint32_t st_rtt2_n = 0, st_rtt2_sum = 0, st_rtt2_min = 0xFFFFFFFF, st_rtt2_max = 0;
static uint32_t st_rtt3_n = 0, st_rtt3_sum = 0, st_rtt3_min = 0xFFFFFFFF, st_rtt3_max = 0;

/* ------------------------ 6. HMAC-SHA256 (Q1+Q2) --------------------- */
static void hmac_tag(uint32_t counter_n, uint32_t r, uint8_t out32[32]) {
  uint8_t msg[8];
  msg[0]=(uint8_t)(counter_n>>24); msg[1]=(uint8_t)(counter_n>>16);
  msg[2]=(uint8_t)(counter_n>>8);  msg[3]=(uint8_t)(counter_n);
  msg[4]=(uint8_t)(r>>24); msg[5]=(uint8_t)(r>>16);
  msg[6]=(uint8_t)(r>>8);  msg[7]=(uint8_t)(r);
  mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, K_MASTER, 32);
  mbedtls_md_hmac_update(&ctx, msg, 8);
  mbedtls_md_hmac_finish(&ctx, out32);
  mbedtls_md_free(&ctx);
}

/* ------------------------ 7. BẪY R3 — DỰ TRỮ BỘ ĐẾM ------------------
 * BẤT BIẾN (đây là điều phải chứng minh, không phải điều mong đợi):
 *   Gọi V là giá trị đang nằm trong NVS. Firmware bảo đảm:
 *     (a) Khi khởi động, counter đầu tiên dùng là V + S  và NVS được ghi
 *         ngay thành V + S.
 *     (b) Trong lúc chạy, NVS được ghi lại mỗi khi counter chia hết cho S.
 *   Từ (a) và (b): giữa hai lần ghi liên tiếp, counter chỉ tăng thêm đúng S,
 *   nên counter lớn nhất TỪNG DÙNG luôn < V + S. Lần khởi động sau bắt đầu
 *   tại V + S, tức LỚN HƠN mọi counter đã dùng. Không bao giờ lặp lại — kể
 *   cả khi mất điện đột ngột.
 *   Cái giá: mỗi lần khởi động "đốt" tối đa S giá trị counter. Với S = 1000
 *   và không gian 2^32, phải khởi động 4,29 triệu lần mới cạn.
 *   ĐIỂM YẾU THẬT SỰ: nếu NVS ghi hỏng (flash mòn) thì bất biến sụp. TN17 in
 *   st_nvs_writes để ước lượng số chu kỳ ghi.
 * --------------------------------------------------------------------- */
static void nvs_mo_va_dat_cho(uint32_t stride) {
  g_nvs.begin("cpe", false);
  uint32_t saved = g_nvs.getUInt("cnt", 0);
  g_boot_id      = g_nvs.getUInt("boot", 0) + 1;
  g_nvs.putUInt("boot", g_boot_id);
  g_counter      = saved + stride;
  g_nvs.putUInt("cnt", g_counter);
  st_nvs_writes += 2;
  g_counter_dau  = g_counter;
  /* Dòng này là ĐẦU VÀO của tn17_phan_tich.py — đừng đổi định dạng. */
  Serial.printf("# R3,boot_id=%lu,nvs_cu=%lu,stride=%lu,counter_bat_dau=%lu\n",
                (unsigned long)g_boot_id, (unsigned long)saved,
                (unsigned long)stride, (unsigned long)g_counter);
}
static inline void nvs_cap_nhat_neu_can(void) {
  if ((g_counter % g_stride) == 0) { g_nvs.putUInt("cnt", g_counter); st_nvs_writes++; }
}

/* ------------------------ 8. NHẬT KÝ BỘ NHỚ -------------------------- *
 * TN16 chỉ in HEAP THẤP NHẤT một lần ở cuối. Một con số duy nhất không
 * phân biệt được ba tình huống rất khác nhau:
 *   - heap tụt rồi ổn định  -> bình thường, chỉ là bộ đệm của thư viện
 *   - heap tụt đều theo thời gian -> RÒ RỈ thật
 *   - heap không tụt nhưng KHỐI TRỐNG LỚN NHẤT tụt -> PHÂN MẢNH, sẽ chết sau
 * Vì vậy TN17 in cả bốn con số theo chu kỳ để vẽ được đường.            */
static void ghi_nhat_ky_bo_nho(uint32_t t_s) {
  uint32_t hn  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t hnb = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  uint32_t hp  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  uint32_t hpb = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  if (hn < st_heap_min)  st_heap_min = hn;
  if (hp < st_psram_min) st_psram_min = hp;
  Serial.printf("# HEAP,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
                (unsigned long)t_s, (unsigned long)hn, (unsigned long)hnb,
                (unsigned long)hp, (unsigned long)hpb,
                (unsigned long)st_wifi_down_ms, (unsigned long)st_mqtt_down_ms);
}

/* ------------------------ 9. GOM JPEG (bẫy R2) ----------------------- */
static size_t jpg_sink(void *arg, size_t index, const void *data, size_t len) {
  (void)arg;
  if (index + len > 32768) return 0;            // tràn -> báo lỗi
  memcpy(g_jbuf + index, data, len);
  if (index + len > g_jlen) g_jlen = index + len;
  return len;
}

/* ------------------------ 10. MQTT ----------------------------------- */
static void on_mqtt(char *topic, uint8_t *pl, unsigned int len) {
#if ECHO_ENABLE
  if (len < 5) return;
  if (strcmp(topic, g_topic_echoback) != 0) return;
  uint32_t n = ((uint32_t)pl[0]<<24)|((uint32_t)pl[1]<<16)|((uint32_t)pl[2]<<8)|pl[3];
  uint8_t stage = pl[4];
  uint32_t now = micros();
  for (int i = 0; i < ECHO_SLOTS; i++) {
    if (ec_used[i] && ec_cnt[i] == n) {
      uint32_t rtt = now - ec_t0[i];
      if (stage == 0) {
        st_rtt_n++; st_rtt_sum += rtt;
        if (rtt < st_rtt_min) st_rtt_min = rtt;
        if (rtt > st_rtt_max) st_rtt_max = rtt;
      } else if (stage == 1) {
        st_rtt2_n++; st_rtt2_sum += rtt;
        if (rtt < st_rtt2_min) st_rtt2_min = rtt;
        if (rtt > st_rtt2_max) st_rtt2_max = rtt;
      } else {
        st_rtt3_n++; st_rtt3_sum += rtt;
        if (rtt < st_rtt3_min) st_rtt3_min = rtt;
        if (rtt > st_rtt3_max) st_rtt3_max = rtt;
      }
      return;
    }
  }
#endif
}

/* ---------- KẾT NỐI LẠI KHÔNG CHẶN (thay wifi_ensure() của TN16) ------
 * TN16: while (WiFi.status() != WL_CONNECTED && millis()-t0 < 15000) delay(200);
 *       gọi ngay trong loop(). Mất WiFi một lần là loop() đứng 15 giây,
 *       mất 15 khung, và cột fps đo được KHÔNG còn nghĩa. TN17 thay bằng
 *       máy trạng thái: mỗi vòng loop chỉ tốn vài micro giây, thời gian chờ
 *       tăng dần 1-2-4-8-16-30 giây, và thời gian mất kết nối được CỘNG DỒN
 *       để báo cáo chứ không bị giấu đi.                                 */
static uint8_t  net_wifi_ok = 0, net_mqtt_ok = 0;
static uint32_t net_wifi_thu_ke = 0, net_mqtt_thu_ke = 0;   // mốc millis
static uint16_t net_wifi_cho_s = 1, net_mqtt_cho_s = 1;     // backoff hiện tại
static uint32_t net_wifi_mat_tu = 0, net_mqtt_mat_tu = 0;   // millis lúc mất

static void mang_khoi_tao(void) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);   // TN17 tự quản, để đếm được số lần
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  net_wifi_mat_tu = millis();
  net_wifi_thu_ke = millis() + 1000;
}
static bool mqtt_thu_ket_noi(void) {
  char cid[32]; snprintf(cid, sizeof(cid), "cpe-%s-%lu", DEV_ID, (unsigned long)millis());
  bool ok = g_mqtt.connect(cid, NULL, NULL, g_topic_lwt, 1, true, "offline");
  if (ok) {
    g_wifi.setNoDelay(true);      // TCP_NODELAY phải đặt LẠI sau mỗi lần nối
    g_mqtt.publish(g_topic_lwt, "online", true);
#if ECHO_ENABLE
    g_mqtt.subscribe(g_topic_echoback, 0);
#endif
  }
  return ok;
}
/* Gọi mỗi vòng loop. KHÔNG bao giờ chặn. */
static void mang_phuc_vu(void) {
  uint32_t now = millis();

  /* --- tầng WiFi --- */
  if (WiFi.status() == WL_CONNECTED) {
    if (!net_wifi_ok) {
      net_wifi_ok = 1; net_wifi_cho_s = 1;
      uint32_t d = now - net_wifi_mat_tu;
      st_wifi_down_ms += d;
      if (d > st_wifi_down_max) st_wifi_down_max = d;
      Serial.printf("# WIFI_LEN,t_ms=%lu,gian_doan_ms=%lu,rssi=%d,ip=%s\n",
                    (unsigned long)now, (unsigned long)d, WiFi.RSSI(),
                    WiFi.localIP().toString().c_str());
    }
  } else {
    if (net_wifi_ok) {
      net_wifi_ok = 0; net_wifi_mat_tu = now; st_wifi_reconn++;
      net_mqtt_ok = 0;                      // mất WiFi thì MQTT chắc chắn đứt
      net_mqtt_mat_tu = now;
      net_wifi_cho_s = 1; net_wifi_thu_ke = now;
      Serial.printf("# WIFI_MAT,t_ms=%lu\n", (unsigned long)now);
    }
    if ((int32_t)(now - net_wifi_thu_ke) >= 0) {
      WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS);
      net_wifi_thu_ke = now + (uint32_t)net_wifi_cho_s * 1000UL;
      if (net_wifi_cho_s < 30) net_wifi_cho_s = (net_wifi_cho_s * 2 > 30) ? 30 : net_wifi_cho_s * 2;
    }
    return;                                  // không có WiFi thì khỏi bàn MQTT
  }

  /* --- tầng MQTT --- */
  if (g_mqtt.connected()) {
    if (!net_mqtt_ok) {
      net_mqtt_ok = 1; net_mqtt_cho_s = 1;
      uint32_t d = now - net_mqtt_mat_tu;
      st_mqtt_down_ms += d;
      if (d > st_mqtt_down_max) st_mqtt_down_max = d;
      Serial.printf("# MQTT_LEN,t_ms=%lu,gian_doan_ms=%lu\n",
                    (unsigned long)now, (unsigned long)d);
    }
    g_mqtt.loop();
  } else {
    if (net_mqtt_ok) {
      net_mqtt_ok = 0; net_mqtt_mat_tu = now; st_mqtt_reconn++;
      net_mqtt_cho_s = 1; net_mqtt_thu_ke = now;
      Serial.printf("# MQTT_MAT,t_ms=%lu,state=%d\n", (unsigned long)now, g_mqtt.state());
    }
    if ((int32_t)(now - net_mqtt_thu_ke) >= 0) {
      if (!mqtt_thu_ket_noi()) {
        net_mqtt_thu_ke = now + (uint32_t)net_mqtt_cho_s * 1000UL;
        if (net_mqtt_cho_s < 30) net_mqtt_cho_s = (net_mqtt_cho_s * 2 > 30) ? 30 : net_mqtt_cho_s * 2;
      }
    }
  }
}

/* ------------------------ 11. IN DANH TÍNH NỀN TẢNG ------------------ *
 * ĐÂY LÀ LÝ DO CHÍNH CỦA TN17. Bản V2.1 của đồ án phải thêm một đoạn
 * "Lưu ý về xung nhịp CPU" vì KHÔNG BIẾT lần đo TN15 chạy ở tần số nào.
 * Từ nay mọi log đều tự khai báo tần số, nên chuyện đó không lặp lại.   */
static void in_danh_tinh(void) {
  rtc_cpu_freq_config_t cf; rtc_clk_cpu_freq_get_config(&cf);
  Serial.printf("# NENTANG,cpu_mhz=%lu,xtal_mhz=%lu,apb_hz=%lu\n",
                (unsigned long)getCpuFrequencyMhz(),
                (unsigned long)getXtalFrequencyMhz(),
                (unsigned long)getApbFrequency());
  Serial.printf("# NENTANG,rtc_cfg_source_freq_mhz=%lu,div=%lu,freq_mhz=%lu\n",
                (unsigned long)cf.source_freq_mhz, (unsigned long)cf.div,
                (unsigned long)cf.freq_mhz);
  Serial.printf("# NENTANG,sdk=%s,chip_rev=%d,cores=%d,flash_hz=%lu\n",
                ESP.getSdkVersion(), (int)ESP.getChipRevision(),
                (int)ESP.getChipCores(), (unsigned long)ESP.getFlashChipSpeed());
  Serial.printf("# NENTANG,ly_do_reset=%d,heap_khoi_dong=%lu,psram=%lu\n",
                (int)esp_reset_reason(),
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ------------------------ 12. VÂN TAY BẮT BUỘC ----------------------- */
static void in_van_tay(uint16_t *perm_buf) {
  uint8_t T[32]; hmac_tag(0, 0, T);
  Serial.print("# VANTAY,hmac_T0=");
  for (int i = 0; i < 8; i++) Serial.printf("%02x", T[i]);
  Serial.println("  (PC: 9f0cd9b94097fe49)");
  perm_C_from_tag(T, perm_buf, NBLK);
  Serial.print("# VANTAY,perm0_dau8=");
  for (int i = 0; i < 8; i++) Serial.printf("%d ", perm_buf[i]);
  Serial.println("  (PC: 109 1171 266 101 493 889 1106 419)");
  Serial.printf("# VANTAY,fnv_n0=0x%016llX   (PC: 0xE4D8CAD02C139271)\n",
                perm_fnv(perm_buf, NBLK));
  Serial.printf("# VANTAY,crc16_123456789=0x%04X   (PC: 0x29B1)\n",
                crc16_ccitt_false((const uint8_t*)"123456789", 9));
  uint8_t hdr[12];
  build_header(hdr, 0x06, 0x03, 0x12345678, 0x1290, 0xA61C);
  Serial.print("# VANTAY,header_mau=");
  for (int i = 0; i < 12; i++) Serial.printf("%02x", hdr[i]);
  Serial.println("  (PC: 43010603123456781290a61c)");
  hmac_tag(12345, 0, T); perm_C_from_tag(T, perm_buf, NBLK);
  Serial.printf("# VANTAY,fnv_n12345=0x%016llX   (PC: 0x77CDD08E5E99A22D)\n",
                perm_fnv(perm_buf, NBLK));
  /* HAI VÂN TAY MỚI CỦA TN17 — lần đầu kiểm cả đường ẢNH, không chỉ hoán vị.
     Cần 2 x 76800 B nên chỉ chạy được khi đã có PSRAM. */
  if (psramFound()) {
    uint8_t *a = (uint8_t*)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *b = (uint8_t*)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (a && b) {
      xorshift32_image(a, FRAME_BYTES, 0x12345678u);
      Serial.printf("# VANTAY,fnv_anh_mau=0x%016llX   (PC: 0x09D571834575A14F)\n",
                    fnv1a64(a, FRAME_BYTES));
      hmac_tag(0, 0, T); perm_C_from_tag(T, perm_buf, NBLK);
      permute_blocks(a, b, perm_buf);
      Serial.printf("# VANTAY,fnv_anh_hoanvi_n0=0x%016llX   (PC: 0x45F79AACCB48E44B)\n",
                    fnv1a64(b, FRAME_BYTES));
    }
    if (a) heap_caps_free(a);
    if (b) heap_caps_free(b);
  }
  Serial.println("# >>> NEU 7 VAN TAY TREN KHONG KHOP, DUNG LAI. Moi so sau do vo gia tri.");
}

/* ===================================================================== */
/* ====================== CHE_DO 2 — KIỂM BẪY R3 ======================= */
/* ===================================================================== */
#if CHE_DO == 2
void setup() {
  Serial.begin(115200); delay(500);
  Serial.println();
  Serial.println("# ===== TN17 CHE_DO 2 — KIEM BAY R3 (khong can camera/WiFi) =====");
  in_danh_tinh();
  g_stride = CNT_STRIDE_R3;
  nvs_mo_va_dat_cho(g_stride);

  /* Tiêu thụ KHUNG_MOI_LAN giá trị counter y như lúc chạy thật:
     mỗi "khung" tăng counter và ghi NVS khi chia hết stride. */
  for (uint32_t i = 0; i < KHUNG_MOI_LAN; i++) {
    Serial.printf("KHUNG,%lu,%lu\n", (unsigned long)i, (unsigned long)g_counter);
    g_counter++;
    nvs_cap_nhat_neu_can();
    delay(5);
  }
  Serial.printf("# R3_KET,boot_id=%lu,counter_dau=%lu,counter_cuoi_da_dung=%lu,nvs_hien=%lu,so_lan_ghi_nvs=%lu\n",
                (unsigned long)g_boot_id, (unsigned long)g_counter_dau,
                (unsigned long)(g_counter - 1),
                (unsigned long)g_nvs.getUInt("cnt", 0), (unsigned long)st_nvs_writes);

  if (g_boot_id < SO_LAN_RESET) {
    Serial.printf("# R3_RESET,lan=%lu/%d — tu khoi dong lai sau 1 giay\n",
                  (unsigned long)g_boot_id, SO_LAN_RESET);
    Serial.flush(); delay(1000);
    esp_restart();                       // mô phỏng MẤT ĐIỆN ĐỘT NGỘT
  }
  Serial.println("# R3_XONG — chay tn17_phan_tich.py --r3 tren log nay");
  Serial.println("# LUU Y: muon chay lai tu dau phai XOA NVS (nap sketch co g_nvs.clear()"
                 " hoac dung 'Erase Flash: All Flash Contents' trong Arduino IDE)");
}
void loop() { delay(1000); }

/* ===================================================================== */
/* ====================== CHE_DO 1 — CHẠY DÀI THẬT ===================== */
/* ===================================================================== */
#else

static bool do_one_frame(uint32_t n) {
  uint32_t t_cap=0, t_hmac=0, t_perm=0, t_jpeg=0, t_pub=0, ta;
  uint32_t t_all0 = micros();

  ta = micros();
  camera_fb_t *fb = esp_camera_fb_get();
  t_cap = micros() - ta;
  if (!fb) { st_fail_cap++; Serial.printf("%lu,,,,,,,,,,,,,CAP_FAIL\n", (unsigned long)n); return false; }
  if (fb->len != FRAME_BYTES) {
    Serial.printf("# CANH BAO: fb->len = %u, ky vong %d\n", (unsigned)fb->len, FRAME_BYTES);
  }

  uint32_t cn = g_counter;
  uint8_t  rr = 0;                      /* PP-C: KHÔNG có ShortCycleError -> r luôn 0 */
  uint8_t  T[32];

  ta = micros(); hmac_tag(cn, rr, T); t_hmac = micros() - ta;

  ta = micros();
#if PERM_MODE == 2
  perm_C_from_tag(T, g_perm, NBLK);
  permute_blocks(fb->buf, g_permed, g_perm);
  const uint8_t *src = g_permed;
#else
  const uint8_t *src = fb->buf;         /* PERM_MODE 0: đối chứng không mã hóa */
#endif
  t_perm = micros() - ta;

  ta = micros();
  g_jlen = 0;
  bool okj = fmt2jpg_cb((uint8_t*)src, FRAME_BYTES, W_IMG, H_IMG,
                        PIXFORMAT_GRAYSCALE, JPEG_Q, jpg_sink, NULL);
  t_jpeg = micros() - ta;
  esp_camera_fb_return(fb);
  if (!okj || g_jlen == 0 || g_jlen > 65535) {
    st_fail_jpg++; Serial.printf("%lu,%lu,,,,,,,,,,,,JPEG_FAIL\n",
                                 (unsigned long)n, (unsigned long)cn); return false;
  }

  uint8_t flags = (uint8_t)((PERM_MODE & 0x03) | ((1 & 0x07) << 2));  /* idx Q=20 là 1 */
  uint8_t hdr[12];
  build_header(hdr, flags, rr, cn, (uint16_t)g_jlen, 0);
  uint16_t crc_all = crc16_ccitt_false(hdr, 10);
  { uint16_t c = crc_all; for (uint32_t i=0;i<g_jlen;i++)
      c = (uint16_t)((c << 8) ^ g_crc_tab[((c >> 8) ^ g_jbuf[i]) & 0xFF]);
    crc_all = c; }
  hdr[10] = (uint8_t)(crc_all >> 8); hdr[11] = (uint8_t)(crc_all);

#if ECHO_ENABLE
  /* THỨ TỰ QUAN TRỌNG (TN16): echo 4 byte phải publish TRƯỚC khung ~3 KB,
     nếu không chặng 2 xếp hàng sau tải tin và đo ra số lớn hơn chặng 0. */
  {
    int slot = n % ECHO_SLOTS;
    ec_cnt[slot] = cn; ec_t0[slot] = micros(); ec_used[slot] = 1;
    uint8_t e[4] = { (uint8_t)(cn>>24),(uint8_t)(cn>>16),(uint8_t)(cn>>8),(uint8_t)cn };
    if (g_mqtt.connected()) g_mqtt.publish(g_topic_echo, e, 4, false);
  }
#endif

  ta = micros();
  bool okp = false;
  if (g_mqtt.connected()) {
    /* bẫy R1: KHÔNG dùng publish() — gửi theo dòng */
    if (g_mqtt.beginPublish(g_topic_frame, 12 + g_jlen, false)) {
      g_mqtt.write(hdr, 12);
      uint32_t off = 0;
      while (off < g_jlen) {
        uint32_t chunk = (g_jlen - off > 1024) ? 1024 : (g_jlen - off);
        g_mqtt.write(g_jbuf + off, chunk);
        off += chunk;
      }
      okp = g_mqtt.endPublish();
    }
  }
  t_pub = micros() - ta;
  if (!okp) st_fail_pub++;

  uint32_t t_all = micros() - t_all0;
  uint32_t hn = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t hp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (hn < st_heap_min)  st_heap_min = hn;
  if (hp < st_psram_min) st_psram_min = hp;

  Serial.printf("%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%d,%lu,%lu,%s\n",
    (unsigned long)n, (unsigned long)cn,
    (unsigned long)t_cap, (unsigned long)t_hmac, (unsigned long)t_perm,
    (unsigned long)t_jpeg, (unsigned long)t_pub, (unsigned long)t_all,
    (unsigned long)g_jlen, (unsigned long)(g_jlen + 12),
    WiFi.RSSI(), (unsigned long)hn, (unsigned long)hp, okp ? "OK" : "PUB_FAIL");

  if (okp) st_sent++;
  g_counter++;
  nvs_cap_nhat_neu_can();
  return okp;
}

void setup() {
  Serial.begin(115200); delay(400);
  Serial.println();
  Serial.println("# ===== TN17 — FIRMWARE CPE + MQTT (ben vung) =====");

#if CPU_MHZ_MONG_MUON > 0
  setCpuFrequencyMhz(CPU_MHZ_MONG_MUON);      // ÉP tần số, rồi IN RA để đối chứng
#endif
  in_danh_tinh();
  Serial.printf("# CAUHINH,xclk_hz=%d,q=%d,perm_mode=%d,chu_ky_ms=%d,phut=%d,stride=%d\n",
                XCLK_HZ, JPEG_Q, PERM_MODE, FPS_PERIOD_MS, RUN_MINUTES, CNT_STRIDE);

  crc16_init_table();
  if (!psramFound()) { Serial.println("!! KHONG CO PSRAM — DUNG"); while (1) delay(1000); }

  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0=Y2_GPIO_NUM; c.pin_d1=Y3_GPIO_NUM; c.pin_d2=Y4_GPIO_NUM; c.pin_d3=Y5_GPIO_NUM;
  c.pin_d4=Y6_GPIO_NUM; c.pin_d5=Y7_GPIO_NUM; c.pin_d6=Y8_GPIO_NUM; c.pin_d7=Y9_GPIO_NUM;
  c.pin_xclk=XCLK_GPIO_NUM; c.pin_pclk=PCLK_GPIO_NUM; c.pin_vsync=VSYNC_GPIO_NUM;
  c.pin_href=HREF_GPIO_NUM; c.pin_sccb_sda=SIOD_GPIO_NUM; c.pin_sccb_scl=SIOC_GPIO_NUM;
  c.pin_pwdn=PWDN_GPIO_NUM; c.pin_reset=RESET_GPIO_NUM;
  c.xclk_freq_hz = XCLK_HZ;
  c.pixel_format = PIXFORMAT_GRAYSCALE;
  c.frame_size   = FRAMESIZE_QVGA;
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;
  if (esp_camera_init(&c) != ESP_OK) { Serial.println("!! esp_camera_init LOI"); while (1) delay(1000); }

  g_perm   = (uint16_t*)heap_caps_malloc(NBLK * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_permed = (uint8_t*) heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
  g_jbuf   = (uint8_t*) heap_caps_malloc(32768,       MALLOC_CAP_SPIRAM);
  if (!g_perm || !g_permed || !g_jbuf) { Serial.println("!! KHONG CAP PHAT DUOC"); while (1) delay(1000); }

  g_stride = CNT_STRIDE;
  nvs_mo_va_dat_cho(g_stride);
  in_van_tay(g_perm);

  snprintf(g_topic_frame,    sizeof(g_topic_frame),    "cpe/%s/frame",    DEV_ID);
  snprintf(g_topic_stat,     sizeof(g_topic_stat),     "cpe/%s/stat",     DEV_ID);
  snprintf(g_topic_lwt,      sizeof(g_topic_lwt),      "cpe/%s/lwt",      DEV_ID);
  snprintf(g_topic_echo,     sizeof(g_topic_echo),     "cpe/%s/echo",     DEV_ID);
  snprintf(g_topic_echoback, sizeof(g_topic_echoback), "cpe/%s/echoback", DEV_ID);

  g_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  g_mqtt.setCallback(on_mqtt);
  g_mqtt.setSocketTimeout(5);
  g_mqtt.setKeepAlive(30);
  mang_khoi_tao();
  /* Chờ CÓ GIỚI HẠN ở setup (chỉ ở setup, không bao giờ trong loop) */
  uint32_t t0 = millis();
  while (millis() - t0 < 20000 && !(net_wifi_ok && net_mqtt_ok)) { mang_phuc_vu(); delay(20); }
  Serial.printf("# KHOIDONG,wifi=%d,mqtt=%d,cho_ms=%lu\n",
                net_wifi_ok, net_mqtt_ok, (unsigned long)(millis() - t0));

  /* bỏ 3 khung ấm — TN15 đã bị artifact vì quên bước này */
  for (int i = 0; i < 3; i++) { camera_fb_t *fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb); }

  Serial.println("# CSV_HEAD,n,counter,t_cap_us,t_hmac_us,t_perm_us,t_jpeg_us,t_pub_us,"
                 "t_tong_us,len_jpeg,len_msg,rssi,heap_noi,psram,ket_qua");
}

static uint32_t g_n = 0;
static uint32_t g_t_next = 0;
static uint32_t g_t_start = 0;
static uint32_t g_t_heap_next = 0;
static bool     g_done = false;

void loop() {
  if (g_done) { delay(1000); return; }
  if (g_t_start == 0) { g_t_start = millis(); g_t_next = g_t_start; g_t_heap_next = g_t_start; }

  mang_phuc_vu();                 // KHÔNG CHẶN

  uint32_t now = millis();

  if ((int32_t)(now - g_t_heap_next) >= 0) {
    g_t_heap_next += (uint32_t)HEAP_LOG_S * 1000UL;
    ghi_nhat_ky_bo_nho((now - g_t_start) / 1000);
  }

  if ((int32_t)(now - g_t_next) >= 0) {
    g_t_next += FPS_PERIOD_MS;
    if ((int32_t)(millis() - g_t_next) >= 0) {
      st_tre_nhip++;
      Serial.printf("# TRE_NHIP tai n=%lu\n", (unsigned long)g_n);
      g_t_next = millis() + FPS_PERIOD_MS;
    }
    do_one_frame(g_n);
    g_n++;
  }

  if (millis() - g_t_start >= (uint32_t)RUN_MINUTES * 60000UL) {
    g_done = true;
    uint32_t secs = (millis() - g_t_start) / 1000;
    ghi_nhat_ky_bo_nho(secs);
    Serial.println("# ================= TONG KET TN17 =================");
    Serial.printf("# cpu_mhz,%lu\n", (unsigned long)getCpuFrequencyMhz());
    Serial.printf("# thoi_gian_chay_s,%lu\n", (unsigned long)secs);
    Serial.printf("# khung_dinh_gui,%lu\n",  (unsigned long)g_n);
    Serial.printf("# khung_gui_thanh_cong,%lu\n", (unsigned long)st_sent);
    Serial.printf("# loi_chup,%lu\n", (unsigned long)st_fail_cap);
    Serial.printf("# loi_nen,%lu\n",  (unsigned long)st_fail_jpg);
    Serial.printf("# loi_publish,%lu\n", (unsigned long)st_fail_pub);
    Serial.printf("# tre_nhip,%lu\n", (unsigned long)st_tre_nhip);
    Serial.printf("# wifi_dut_lan,%lu\n", (unsigned long)st_wifi_reconn);
    Serial.printf("# wifi_gian_doan_tong_ms,%lu\n", (unsigned long)st_wifi_down_ms);
    Serial.printf("# wifi_gian_doan_lau_nhat_ms,%lu\n", (unsigned long)st_wifi_down_max);
    Serial.printf("# mqtt_dut_lan,%lu\n", (unsigned long)st_mqtt_reconn);
    Serial.printf("# mqtt_gian_doan_tong_ms,%lu\n", (unsigned long)st_mqtt_down_ms);
    Serial.printf("# mqtt_gian_doan_lau_nhat_ms,%lu\n", (unsigned long)st_mqtt_down_max);
    Serial.printf("# fps_dat_duoc,%lu.%03lu\n",
                  (unsigned long)(st_sent / (secs ? secs : 1)),
                  (unsigned long)((st_sent * 1000UL / (secs ? secs : 1)) % 1000));
    Serial.printf("# heap_noi_thap_nhat,%lu\n",  (unsigned long)st_heap_min);
    Serial.printf("# psram_thap_nhat,%lu\n",     (unsigned long)st_psram_min);
    Serial.printf("# counter_dau,%lu\n", (unsigned long)g_counter_dau);
    Serial.printf("# counter_cuoi_da_dung,%lu\n", (unsigned long)(g_counter - 1));
    Serial.printf("# nvs_hien,%lu\n", (unsigned long)g_nvs.getUInt("cnt", 0));
    Serial.printf("# so_lan_ghi_nvs,%lu\n", (unsigned long)st_nvs_writes);
    Serial.printf("# boot_id,%lu\n", (unsigned long)g_boot_id);
#if ECHO_ENABLE
    if (st_rtt_n)  Serial.printf("# rtt_vua_nhan_us,n=%lu,tb=%lu,min=%lu,max=%lu\n",
        (unsigned long)st_rtt_n, (unsigned long)(st_rtt_sum/st_rtt_n),
        (unsigned long)st_rtt_min, (unsigned long)st_rtt_max);
    else Serial.println("# rtt_vua_nhan_us,CHUA DO (khong nhan duoc echoback)");
    if (st_rtt2_n) Serial.printf("# rtt_giai_ma_xong_us,n=%lu,tb=%lu,min=%lu,max=%lu\n",
        (unsigned long)st_rtt2_n, (unsigned long)(st_rtt2_sum/st_rtt2_n),
        (unsigned long)st_rtt2_min, (unsigned long)st_rtt2_max);
    else Serial.println("# rtt_giai_ma_xong_us,CHUA DO");
    if (st_rtt3_n) Serial.printf("# rtt_mang_thuan_us,n=%lu,tb=%lu,min=%lu,max=%lu\n",
        (unsigned long)st_rtt3_n, (unsigned long)(st_rtt3_sum/st_rtt3_n),
        (unsigned long)st_rtt3_min, (unsigned long)st_rtt3_max);
    else Serial.println("# rtt_mang_thuan_us,CHUA DO");
#endif
    Serial.println("# ================================================");
    char st[200];
    snprintf(st, sizeof(st),
             "{\"sent\":%lu,\"secs\":%lu,\"failpub\":%lu,\"wifidrop\":%lu,\"mqttdrop\":%lu,\"cpu\":%lu}",
             (unsigned long)st_sent, (unsigned long)secs, (unsigned long)st_fail_pub,
             (unsigned long)st_wifi_reconn, (unsigned long)st_mqtt_reconn,
             (unsigned long)getCpuFrequencyMhz());
    g_mqtt.publish(g_topic_stat, st, true);
    g_mqtt.publish(g_topic_lwt, "offline", true);
  }
}
#endif
