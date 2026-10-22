# Giao thức TCP — phase 1

TCP/IPv4. Frame gồm 4 byte unsigned big-endian (độ dài body theo byte, không tính header), tiếp theo là JSON UTF-8. Body từ 1 đến 65.536 byte. Không có newline hoặc NUL kết thúc frame.

Phần socket, framing và xử lý giao thức ứng dụng phải viết bằng C ở cả client và server. GUI có thể dùng C++/Qt nhưng phải gọi module C; không dùng `QTcpSocket` thay lớp socket C. Test harness Python chỉ là công cụ kiểm thử.

Mỗi kết nối mang nhiều request. Header/body có thể bị chia hoặc gộp bởi TCP. Response trả theo thứ tự request trên cùng kết nối; chưa có event. Client phải ghép frame và đối chiếu `request_id`.

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
| `RESPONSE_TOO_LARGE` | Response vượt giới hạn frame; giữ kết nối. ID null. |
| `INTERNAL_ERROR` | Không khởi tạo được parser. ID null. |

Client phải xử lý được ID null. Khi mất kết nối, kết quả request đang chờ là chưa biết; các phase sau cần chính sách chống trùng trước khi tự retry thao tác thay đổi dữ liệu.

## Giới hạn

- Tối đa 64 client; kết nối vượt giới hạn bị đóng.
- Buffer nhận mỗi client: 65.540 byte; hàng đợi gửi: 262.160 byte. Tràn hàng đợi thì ngắt client.
- Frame đang nhận hoặc output đang chờ không có tiến triển I/O trong 10 giây bị ngắt; kiểm tra mỗi 250 ms. Kết nối nhàn rỗi không có frame dở được giữ.
- EOF giữa frame bỏ frame dở. Half-close phía gửi vẫn cho phép đọc các response đã xếp hàng.
- Log stderr: UTC, loại sự kiện, file descriptor; không ghi payload. fd có thể tái sử dụng.

## Phase sau

Giữ framing/envelope, thêm handler tài khoản/phòng rồi luật đấu giá. Chốt API đăng nhập, snapshot, event và schema nghiệp vụ trước phase tương ứng. Phase 1 chưa có xác thực, reconnect, GUI hoặc timer đấu giá.

Luồng nghiệp vụ dự kiến: người tổ chức tạo phòng `DRAFT`, quản lý vật phẩm bằng `ITEM_CREATE/ITEM_UPDATE/ITEM_DELETE/ITEM_REORDER`, rồi `ROOM_PUBLISH`. Khách đã đăng nhập dùng `ROOM_LIST/ROOM_DETAIL/ITEM_LIST/ITEM_SEARCH` xem danh mục đã công bố trước khi `ROOM_JOIN`. Xem trước không chiếm phòng đang tham gia; API phải kiểm tra quyền kể cả truy vấn trực tiếp ID. Bản cơ bản khóa danh mục sau công bố. Các lệnh này chưa được hỗ trợ bởi server phase 1, hiện trả `UNKNOWN_COMMAND`; payload và mã lỗi sẽ được đặc tả trước khi triển khai.
