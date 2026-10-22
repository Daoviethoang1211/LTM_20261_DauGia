# LTM_20261_DauGia

Ứng dụng đấu giá trực tuyến — nền tảng phase 1 chạy trên Linux/Ubuntu WSL.

Đã có TCP server C11 dùng `poll()` và socket không chặn, ghép frame JSON hai phía, client dòng lệnh C, SQLite migration v1 và kiểm thử tích hợp. Tài khoản, phòng, đấu giá và GUI thuộc phase sau. Kế hoạch hiện hành: [PLAN.md](PLAN.md).

Yêu cầu đã chốt: socket/giao thức client và server viết bằng C. GUI được thiết kế tùy ý; nếu dùng Qt, GUI gọi module mạng/giao thức C, không thay bằng `QTcpSocket`. Người tổ chức chuẩn bị và công bố danh mục/lịch trước cho khách xem; khách không tự đăng vật phẩm. Python và shell chỉ phục vụ kiểm thử/điều phối tiến trình, không thay phần giao tiếp C của ứng dụng.

## Build trong Ubuntu WSL

Từ PowerShell mở Ubuntu:

```powershell
wsl -d Ubuntu
```

Các lệnh sau chạy **trong Ubuntu**:

```bash
cd /mnt/c/Users/ADMIN/Downloads/LTM
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libsqlite3-dev libjson-c-dev python3
make
make test
```

`make` hoặc `make all` chỉ **biên dịch**, không chạy server. Thông báo `Nothing to be done for 'all'.` nghĩa là `build/server` và `build/client` đã cập nhật so với mã nguồn, không phải lỗi. Dùng `make run` để chạy server, `make test` để kiểm thử; chỉ cần `make clean && make all` khi muốn build lại toàn bộ.

Chạy nhanh sau khi build, trong hai terminal Ubuntu cùng thư mục dự án:

```bash
# Terminal 1: server, Ctrl+C để dừng
make run
```

```bash
# Terminal 2: ba client C gửi PING đồng thời
make demo
```

Mặc định dùng `127.0.0.1:5555`, CSDL `var/auction.db`. Có thể đổi bằng `make run PORT=5556 DB=var/demo.db` và `make demo PORT=5556`. `make run` in log ra stderr; có thể lưu bằng `make run 2>>var/server.log` sau khi tạo thư mục `var`.

Makefile lấy dependency qua `pkg-config`, không tải thư viện khi build. Môi trường kiểm chứng: json-c 0.18 và SQLite 3.46.1. Trên môi trường này, chế độ strict của json-c từ chối ký tự UTF-8 trực tiếp. Server kiểm tra UTF-8 rồi chuyển ký tự Unicode thành JSON escape trong buffer tạm trước khi parse strict; nội dung gửi/nhận vẫn là UTF-8. json-c dùng giấy phép MIT, SQLite thuộc public domain; link từ gói Ubuntu, không vendor thư viện vào repo. Python dùng thư viện chuẩn để kiểm thử.

## Chạy server và ba client

Terminal Ubuntu thứ nhất:

```bash
cd /mnt/c/Users/ADMIN/Downloads/LTM
mkdir -p var
./build/server 127.0.0.1 5555 var/auction.db 2>>var/server.log
```

Server in `READY 5555` khi sẵn sàng. Đối số: IPv4 bind, cổng, đường dẫn CSDL; thư mục chứa CSDL phải tồn tại. Cổng `0` chọn cổng trống và in cổng thực tế. Ctrl+C dừng server.

Terminal Ubuntu thứ hai chạy ba client đồng thời:

```bash
cd /mnt/c/Users/ADMIN/Downloads/LTM
for i in 1 2 3; do
  ./build/client 127.0.0.1 5555 "{\"type\":\"PING\",\"request_id\":\"demo-$i\",\"payload\":{\"text\":\"Xin chào từ client $i\"}}" &
done
wait
```

Mỗi client nhận JSON chứa PONG và ID tương ứng. Khởi động lại server cùng đường dẫn CSDL: `server_start_count` tăng, chứng minh dữ liệu đã commit vẫn còn. Bind `0.0.0.0` khi cần cho máy khác truy cập.

## Cấu trúc và mở rộng

| File | Trách nhiệm |
| --- | --- |
| `src/server.c` | Event loop, kết nối, buffer hữu hạn, backpressure, timeout, log. |
| `src/frame.*` | Header độ dài dùng chung server và client. |
| `src/protocol.*` | JSON, kiểm tra envelope, điều phối lệnh và response. |
| `src/storage.*` | SQLite, foreign keys, transaction, migration qua `user_version`. |
| `src/client.c` | Gửi một request, nhận response; timeout I/O 5 giây. |
| `tests/integration.py` | Kiểm thử TCP thật, CSDL tạm, tự khởi động/dừng server. |

Wire format, mã lỗi và giới hạn: [docs/protocol.md](docs/protocol.md).

Phase 2 thêm service tài khoản/phòng và migration v2; phase 3 thêm module luật đấu giá và clock/timer gọi từ event loop. Không đặt luật nghiệp vụ vào callback socket. Migration mới phải giữ dữ liệu cũ và chạy trong transaction; server từ chối CSDL có version mới hơn mình hiểu.

Phase 3 theo mô hình người tổ chức: phòng `DRAFT` để chuẩn bị/sắp danh mục → `ROOM_PUBLISH` để khách xem trước → tự mở phiên theo lịch. Quy ước bản cơ bản khóa danh mục sau công bố; chưa có nghiệp vụ này trong code phase 1. Khi làm GUI, tách kết nối, framing và encode/decode thông điệp thành module C với API gọi từ C++; GUI xử lý hiển thị và thao tác người dùng.

Schema phase 1 chỉ có `server_starts(id, started_at)` để nghiệm thu lưu trữ. Chưa tạo bảng nghiệp vụ khi các quyết định trong PLAN còn mở. SQLite chỉ được truy cập lúc khởi động trong phase này; truy vấn và băm mật khẩu ở phase sau phải có ngân sách thời gian hoặc cơ chế xử lý riêng để không chặn event loop lâu.

## Nghiệm thu

`make test`: 3 client đồng thời; frame chia/gộp; đọc response từng byte; tiếng Việt; JSON/UTF-8 sai; request sai; frame quá lớn/rỗng/tối đa; ngắt giữa frame; half-close; client chậm; timeout frame dở; client C; restart thường và kill đột ngột; rollback migration lỗi; từ chối schema tương lai. Test dùng cổng trống và CSDL tạm riêng, không sửa `var/auction.db`.

Đọc từng byte không bảo đảm kernel tạo short write; server xử lý số byte thực tế của `send()`, giữ phần còn lại đến `POLLOUT`. Test client chậm kiểm tra khả năng phục vụ client khác và thu hồi kết nối bị nghẽn.

Build bật `-Wall -Wextra -Wpedantic -Werror`. Kiểm tra bộ nhớ trong WSL:

```bash
bash scripts/check-memory.sh
```

`build/` và `var/` được Git bỏ qua. Build/test không sửa `PLAN.md`.
