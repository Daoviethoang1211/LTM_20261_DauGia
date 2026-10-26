# LTM_20261_DauGia

Ứng dụng đấu giá trực tuyến — bản demo phase 2 chạy trên Linux/Ubuntu WSL.

Đã có tài khoản/phiên, phòng nháp, danh mục do người tổ chức chuẩn bị/công bố, khách xem trước/vào/rời phòng, thông báo thành viên và client menu C. Server C11 dùng `poll()` và socket không chặn; SQLite tự nâng cấp v1→v2. Bid/mua ngay/timer/kết quả và GUI Qt thuộc phase sau. Kế hoạch hiện hành: [PLAN.md](PLAN.md). Kịch bản demo chi tiết: [docs/phase2.md](docs/phase2.md).

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
sudo apt-get install -y build-essential pkg-config libsqlite3-dev libjson-c-dev libsodium-dev python3
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

`make demo` vẫn là kiểm tra PING của phase 1. Để demo chức năng phase 2, giữ `make run` ở terminal server rồi mở ba terminal khác, mỗi terminal chạy:

```bash
make client
```

Menu có đăng ký/đăng nhập, tạo phòng, quản lý/công bố danh mục, xem trước/vào/rời phòng. Xem [kịch bản 3 người](docs/phase2.md). Menu không yêu cầu nhập JSON. Mỗi client giữ một kết nối và nhận thông báo thành viên khi đang chờ nhập.

Mặc định dùng `127.0.0.1:5555`, CSDL `var/auction.db`. Có thể đổi bằng `make run PORT=5556 DB=var/demo.db` và `make demo PORT=5556`. `make run` in log ra stderr; có thể lưu bằng `make run 2>>var/server.log` sau khi tạo thư mục `var`.

Makefile lấy dependency qua `pkg-config`, không tải thư viện khi build. Môi trường kiểm chứng: json-c 0.18, SQLite 3.46.1 và libsodium 1.0.18. Trên môi trường này, chế độ strict của json-c từ chối ký tự UTF-8 trực tiếp. Module giao thức kiểm tra UTF-8 rồi chuyển Unicode thành JSON escape trong buffer tạm trước khi parse strict; nội dung gửi/nhận vẫn là UTF-8. json-c dùng MIT, libsodium dùng ISC, SQLite thuộc public domain; link từ gói Ubuntu, không vendor thư viện vào repo. Python chỉ dùng thư viện chuẩn để kiểm thử.

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
| `src/protocol.*` | JSON, kiểm tra UTF-8/envelope, response và PING tương thích phase 1. |
| `src/storage.*` | SQLite, foreign keys, transaction, migration qua `user_version`. |
| `src/app.*` | Phiên, quyền, tài khoản, phòng/danh mục, snapshot và sự kiện thành viên. Chỉ event loop truy cập DB/phiên. |
| `src/auth_worker.*` | Argon2id ngoài event loop, tối đa 16 yêu cầu auth chưa thu kết quả. |
| `src/client_net.*` | Socket, framing, JSON request/response và callback event trong thư viện C dùng lại. |
| `src/menu.c` | Menu giữ kết nối, nhập mật khẩu ẩn trên terminal, xem danh mục và nhận thông báo. |
| `tests/` | Kiểm thử TCP thật và menu C, CSDL tạm, tự khởi động/dừng server. |

Wire format, mã lỗi và giới hạn: [docs/protocol.md](docs/protocol.md).

Phase 2 thêm tài khoản/phòng/danh mục và migration v2; phase 3 sẽ thêm luật đấu giá/clock/timer và migration tiếp theo. Không đặt luật nghiệp vụ vào callback socket. Migration giữ dữ liệu cũ và chạy trong transaction; server từ chối CSDL có version mới hơn mình hiểu.

Luồng phase 2: phòng `DRAFT` để chuẩn bị/sắp danh mục → `ROOM_PUBLISH` để khách xem trước. Danh mục khóa sau công bố; **chưa tự mở đấu giá khi đến lịch**. `build/libauction_client.a` và `src/client_net.h` là thư viện/API C gọi được từ C++; GUI sau này xử lý hiển thị và gọi thư viện từ worker để không chặn UI.

Schema v2 giữ `server_starts` và bổ sung `users`, `rooms`, `items`; các field bid/kết quả sẽ thêm bằng migration phase 3. Dùng Argon2id có salt qua libsodium; băm/xác minh chạy ở một worker với hàng đợi giới hạn, SQLite/phiên chỉ cập nhật trên event loop. Kết quả auth gắn generation kết nối để tránh áp vào slot đã được tái sử dụng. Request auth trên cùng kết nối được xử lý theo thứ tự; đầy hàng đợi trả `SERVER_BUSY`.

## Nghiệm thu

`make test`: 3 client đồng thời; frame chia/gộp; đọc response từng byte; tiếng Việt; JSON/UTF-8 sai; request sai; frame quá lớn/rỗng/tối đa; ngắt giữa frame; half-close; client chậm; timeout frame dở; client C; restart thường và kill đột ngột; rollback migration lỗi; từ chối schema tương lai. Test dùng cổng trống và CSDL tạm riêng, không sửa `var/auction.db`.

Phase 2 thêm kiểm thử tài khoản/đổi mật khẩu, đăng nhập trùng/cạnh tranh, auth queue, kết nối bị tái sử dụng, ẩn nháp/phân quyền/khóa danh mục, thứ tự vật phẩm, snapshot/sự kiện, giới hạn một phòng, restart giữ danh mục, nâng cấp v1→v2 và thao tác menu C không nhập JSON.

Đọc từng byte không bảo đảm kernel tạo short write; server xử lý số byte thực tế của `send()`, giữ phần còn lại đến `POLLOUT`. Test client chậm kiểm tra khả năng phục vụ client khác và thu hồi kết nối bị nghẽn.

Build bật `-Wall -Wextra -Wpedantic -Werror`. Kiểm tra bộ nhớ trong WSL:

```bash
bash scripts/check-memory.sh
```

`build/` và `var/` được Git bỏ qua. Build/test không sửa `PLAN.md`.
