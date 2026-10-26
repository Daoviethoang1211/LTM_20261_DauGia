# Giao thức TCP — phase 2

TCP/IPv4. Frame gồm 4 byte unsigned big-endian (độ dài body theo byte, không tính header), tiếp theo là JSON UTF-8. Body từ 1 đến 65.536 byte. Không có newline hoặc NUL kết thúc frame.

Phần socket, framing và xử lý giao thức ứng dụng phải viết bằng C ở cả client và server. GUI có thể dùng C++/Qt nhưng phải gọi module C; không dùng `QTcpSocket` thay lớp socket C. Test harness Python chỉ là công cụ kiểm thử.

Mỗi kết nối mang nhiều request. Header/body có thể bị chia hoặc gộp bởi TCP. Response trả theo thứ tự request trên cùng kết nối; event thành viên có thể xen giữa các response. Client phân biệt `event` với response và đối chiếu `request_id`. Khi có auth đang chờ worker, server tạm dừng đọc/xử lý request tiếp theo của kết nối đó, giữ phần frame đã nhận trong buffer hữu hạn.

## Request hiện có

```json
{"type":"PING","request_id":"p1","payload":{"text":"Đấu giá"}}
```

- Root và `payload` phải là JSON object.
- `type` là chuỗi tên lệnh, phân biệt hoa thường, không chứa NUL.
- `request_id` là chuỗi UTF-8 không rỗng, tối đa 128 byte, không chứa NUL. Chưa có chống thực thi request trùng.
- `PING` trả `PONG`, echo payload và tổng số lần khởi động đã commit trong CSDL. Đây là lệnh kiểm thử nền tảng. Không gửi dữ liệu nhạy cảm qua PING.
- Field bổ sung được bỏ qua.

```json
{"request_id":"p1","ok":true,"data":{"message":"PONG","echo":{"text":"Đấu giá"},"server_start_count":1}}
```

## Response lỗi

```json
{"request_id":"p1","ok":false,"error_code":"UNKNOWN_COMMAND"}
```

| Mã | Hành vi |
| --- | --- |
| `INVALID_JSON` | JSON/UTF-8 sai, root không phải object, hoặc dữ liệu dư sau JSON; giữ kết nối. ID null. |
| `INVALID_REQUEST` | Thiếu/sai kiểu field; giữ kết nối. ID null nếu ID không hợp lệ. |
| `UNKNOWN_COMMAND` | Lệnh chưa hỗ trợ; giữ kết nối và trả ID. |
| `INVALID_FRAME_SIZE` | Độ dài 0 hoặc >65.536; cố gửi lỗi rồi đóng kết nối. ID null. |
| `RESPONSE_TOO_LARGE` | Response vượt giới hạn frame; giữ kết nối, giữ ID nếu có. |
| `INTERNAL_ERROR` | Không khởi tạo được parser. ID null. |

Client phải xử lý được ID null. Khi mất kết nối, kết quả request đang chờ là chưa biết; các phase sau cần chính sách chống trùng trước khi tự retry thao tác thay đổi dữ liệu.

## Giới hạn

- Tối đa 64 client; kết nối vượt giới hạn bị đóng.
- Buffer nhận mỗi client: 65.540 byte; hàng đợi gửi: 262.160 byte. Tràn hàng đợi thì ngắt client.
- Frame đang nhận hoặc output đang chờ không có tiến triển I/O trong 10 giây bị ngắt; kiểm tra mỗi 250 ms. Kết nối nhàn rỗi không có frame dở được giữ.
- EOF giữa frame bỏ frame dở. Half-close phía gửi vẫn cho phép đọc các response đã xếp hàng.
- Log stderr: UTC, loại sự kiện, file descriptor; không ghi payload. fd có thể tái sử dụng.

## Phase sau

Phase 3 thêm BID/BUY_NOW và sự kiện timer/kết quả; giữ framing/envelope. Hiện chưa có đấu giá tự động, reconnect, tìm kiếm, lịch sử hoặc GUI. Những lệnh chưa hỗ trợ trả UNKNOWN_COMMAND.

## Lệnh tài khoản đã hỗ trợ

Các lệnh bên dưới dùng cùng envelope `type/request_id/payload` của PING. REGISTER/LOGIN dùng trên kết nối chưa đăng nhập; những lệnh nghiệp vụ khác yêu cầu đăng nhập. PING vẫn dùng được khi chưa đăng nhập.

| Lệnh | Payload | Data khi thành công |
| --- | --- | --- |
| REGISTER | `username`, `password` | `user_id`; chưa tự đăng nhập. |
| LOGIN | `username`, `password` | `user_id`, `username`; gắn danh tính vào kết nối. |
| LOGOUT | `{}` | `{}`; rời phòng và giải phóng phiên, giữ TCP. |
| PASSWORD_CHANGE | `old_password`, `new_password` | `{}`; giữ phiên hiện tại. |

Username 3–32 byte, chỉ `[a-z0-9_]`, phân biệt hoa thường (chữ hoa không hợp lệ). Mật khẩu 8–128 byte UTF-8; không cho NUL/ký tự điều khiển. Lưu chuỗi Argon2id gồm salt/tham số, không trả hash về client. Một tài khoản chỉ có một kết nối đăng nhập. Auth dùng một worker và tối đa 16 job chưa thu kết quả; đầy trả SERVER_BUSY. Hash/xác minh không giữ khóa DB; event loop kiểm tra lại phiên và hash hiện hành trước khi chấp nhận đăng nhập.

## Lệnh phòng đã hỗ trợ

| Lệnh | Payload | Data khi thành công |
| --- | --- | --- |
| ROOM_CREATE | `name`, `description` | Object phòng: `id`, `owner_user_id`, `name`, `description`, `status`, `version`, `published_at`. Tạo DRAFT, không tự vào phòng. |
| ROOM_LIST | `offset` tùy chọn, mặc định 0 | `rooms` tối đa 20 phòng, `next_offset` hoặc null. Gồm nháp của mình và các phòng đã công bố. |
| ROOM_DETAIL | `room_id` | Snapshot gồm `room`, `items`, `members`; chỉ xem, không chiếm phòng. |
| ROOM_SNAPSHOT | `room_id` | Cùng định dạng/quyền với ROOM_DETAIL. |
| ROOM_JOIN | `room_id` | Snapshot; vào tối đa một phòng. Vào lại cùng phòng là idempotent, không tăng version/phát event lần nữa. |
| ROOM_LEAVE | `{}` | `{}`; idempotent nếu chưa ở phòng. |
| ROOM_PUBLISH | `room_id` | Snapshot sau commit. Chỉ chủ phòng DRAFT có danh mục hợp lệ. |

Tên phòng 1–120 byte, mô tả 0–512 byte. Chuỗi văn bản nghiệp vụ không cho NUL hoặc byte điều khiển ASCII. `room_id/item_id` là JSON integer dương, không nhận string/float/bool. Phòng nháp chỉ chủ phòng truy cập, khách biết ID cũng nhận NOT_FOUND. Khách đã đăng nhập xem trước phòng công bố mà không cần ROOM_JOIN. Chủ phòng được vào nháp của mình. Logout/disconnect rời phòng; restart không giữ phiên/thành viên.

`published_at` và `planned_start_at` là Unix timestamp UTC theo giây; null nếu chưa công bố. Công bố yêu cầu ít nhất một vật phẩm, lịch mỗi vật phẩm lớn hơn giờ server và không giảm theo thứ tự. Công bố khóa thông tin/thứ tự danh mục; phase 2 không tự mở phiên khi tới lịch.

## Lệnh vật phẩm đã hỗ trợ

Mọi lệnh có `room_id`. Chỉ chủ phòng DRAFT được thay đổi danh mục; việc quản lý không bắt buộc chủ phòng đang ROOM_JOIN.

| Lệnh | Payload bổ sung | Data khi thành công |
| --- | --- | --- |
| ITEM_CREATE | Các field nội dung bên dưới | `item_id`; thêm cuối hàng. |
| ITEM_UPDATE | `item_id` và toàn bộ field nội dung | `item_id`; giữ thứ tự cũ. |
| ITEM_DELETE | `item_id` | `{}`. ID phải thuộc đúng phòng. |
| ITEM_REORDER | `item_ids`: mảng toàn bộ ID trong phòng theo thứ tự mới | `{}`; không trùng/thiếu/thừa/ID phòng khác. Chạy cùng cập nhật version trong transaction. |
| ITEM_LIST | Không thêm field | `items` theo `queue_position,id`, tối đa 30 vật phẩm. Cùng quyền xem với phòng. |

Field nội dung: `title` (1–120 byte), `description` (0–512 byte), `starting_price_vnd` (integer 0..10^15), `buy_now_price_vnd` (integer lớn hơn giá khởi điểm, tối đa 10^15), `planned_start_at` (integer 1..253402300799), `duration_sec` (integer 30..86400). Nháp cho phép lưu lịch đã cũ để chỉnh lại, nhưng không thể công bố lịch đã qua. Tối đa 30 vật phẩm/phòng để snapshot nằm trong 64 KiB ngay cả khi chuỗi phải JSON-escape và có nhiều thành viên; queue_position có thể có khoảng trống sau xóa, không ảnh hưởng thứ tự; REORDER chuẩn hóa về 1..N. Snapshot trả các field trên cùng `id`, `room_id`, `queue_position`, `status=QUEUED`.

## Snapshot và event thành viên

ROOM_JOIN trả snapshot với version mới; MEMBER_JOINED chỉ gửi cho những người đã ở phòng trước đó. MEMBER_LEFT gửi cho thành viên còn lại khi rời/logout/disconnect. Client dùng snapshot làm trạng thái nền; version lớn hơn phiên bản đang giữ mới được áp dụng. Nếu có khoảng trống version, lấy ROOM_SNAPSHOT để đồng bộ. Version lưu trong DB và tăng khi danh mục/thành viên thay đổi; nó không phải sequence liên tục chỉ dành cho event mạng. Sau restart luôn lấy snapshot mới; không có replay event.

```json
{"event":"MEMBER_JOINED","room_id":1,"version":5,"user_id":2,"username":"guest"}
```

`members` là mảng `{user_id, username}`. Event không có request_id. Đóng socket khi đang auth hủy việc áp dụng kết quả vào phiên cũ; thao tác chưa nhận response không có bảo đảm thành công hay thất bại, không tự retry lệnh thay đổi dữ liệu.

## Mã lỗi nghiệp vụ

- AUTH_REQUIRED, ALREADY_AUTHENTICATED, INVALID_CREDENTIALS, USERNAME_TAKEN, ALREADY_LOGGED_IN.
- NOT_FOUND (bao gồm tài nguyên không được phép nhìn thấy), FORBIDDEN (thấy phòng nhưng không có quyền quản lý).
- ALREADY_IN_ROOM, ROOM_LOCKED, INVALID_CATALOG, CATALOG_FULL.
- SERVER_BUSY (auth queue đầy), DATABASE_ERROR, INVALID_REQUEST.

Lỗi nghiệp vụ giữ kết nối và trả request_id. Log chỉ ghi tên lệnh đã được nhận diện, slot/user/room và kết quả; không ghi payload, mật khẩu hoặc hash.
