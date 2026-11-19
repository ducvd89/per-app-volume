# Per-App Volume

App menu bar cho macOS để chỉnh âm lượng riêng cho từng app (0–200%), giống AppVolume.
**Không dùng micro, không thu âm (process tap), không quay màn hình** → macOS không hiện chấm cam/tím
và không hỏi quyền nào cả.

## Tính năng
- Âm lượng từng app 0–200% (vượt 100% = boost, có soft-clip chống vỡ tiếng), nút tắt tiếng từng app
- Nhớ âm lượng theo app (bundle id), tự áp lại khi app mở lại
- Gộp tiến trình con về app gốc (Chrome Helper → Chrome, WebKit → Safari…)
- Ghim app lên đầu, ẩn app không cần, ⌘-click chọn nhiều app để tắt tiếng / ghim / ẩn / chỉnh âm lượng hàng loạt
- Đồng hồ mức âm (meter) cho từng app
- Âm lượng tổng: phím âm lượng trên bàn phím vẫn hoạt động bình thường (kèm HUD của macOS)
- Chọn thiết bị output: theo thiết bị mặc định của macOS, hoặc theo thứ tự ưu tiên tự đặt
- Sample rate 44.1 kHz – 192 kHz, tự khớp với thiết bị thật
- Phím tắt toàn cục mở bảng điều khiển (mặc định ⌥⌘V) — dùng Carbon hotkey nên không cần quyền Accessibility
- Mở cùng macOS

## Cách hoạt động
```
App (Spotify, Chrome…) ──► thiết bị ảo "Per-App Volume" (HAL driver trong coreaudiod)
                              │  ProcessOutput: nhân gain riêng cho từng client (trước khi mix)
                              │  WriteMix: bản mix → ring buffer
                              ▼
          Per-App Volume.app đọc bản mix qua custom property 'apad'  (KHÔNG mở input stream)
                              │
                              ▼  IOProc chỉ-output
                       Loa / tai nghe thật
```
Vì app không bao giờ mở luồng input hay dùng ScreenCaptureKit / process tap, macOS không coi đó là ghi âm.

## Cài đặt
1. Mở `dist/Per-App Volume.app` (hoặc giải nén `dist/Per-App-Volume.zip`), nên kéo vào **Applications**.
   Lần đầu mở app ký ad-hoc: chuột phải → **Open** → **Open**.
2. Cửa sổ chào mừng hiện ra → bấm **Cài driver…** → nhập mật khẩu admin vào hộp thoại của macOS.
3. Xong. Biểu tượng nằm trên thanh menu (hoặc bấm ⌥⌘V).

Cài lại / cập nhật / gỡ driver: **Cài đặt → Driver âm thanh ảo**. Khi gỡ, output được trả về loa/tai nghe thật.

## Build từ source
Chỉ cần Command Line Tools (không cần Xcode):
```bash
./build.sh      # → dist/Per-App Volume.app và dist/Per-App-Volume.zip
```
Driver được nhúng sẵn trong `Contents/Resources/PerAppVolume.driver`.
(Vẫn có `sudo ./scripts/install-driver.sh` nếu muốn cài bằng Terminal.)

## Cấu trúc
- `Driver/PerAppVolumeDriver.c` — AudioServerPlugIn (thiết bị ảo, gain từng client, ring buffer)
- `App/Engine/PAVEngine.c` — luồng realtime: kéo bản mix từ driver, bù lệch clock, phát ra thiết bị thật
- `App/Sources/` — SwiftUI menu bar app (Router: chọn thiết bị; AppStore: danh sách app + đẩy gain xuống driver)
