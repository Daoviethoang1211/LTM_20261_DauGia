# Kế hoạch project: Ứng dụng đấu giá trực tuyến

## 1. Căn cứ và mục tiêu

- Đề bài: mục 3 “Xây dựng ứng dụng đấu giá trực tuyến” và rubric người dùng cung cấp.
- Tài liệu đã kiểm tra trong Teams, kênh `20161 - Thực hành Lập trình mạng ... / General / Shared`: `Lec01.ReviewCProgramming.pdf`, `Lec03.SocketAPIIntroduction.pdf`, `Lec04.ElementarySocketsProgramming.pdf`, `Lec05.ConcurentServer.pdf`, `Lec06.IOMultiplexing.pdf`, `Lec07.AdvancedIO.pdf`. Slide nội dung của Lec06 liệt kê `select()` và `poll()`. Thư mục `Code tham khảo/TCP` chứa `Makefile`, `tcp_client.c`, `tcp_server.c`; ví dụ server sử dụng `socket` → `bind` → `listen` → `accept` → `recv/send` → `close`.
- Mục tiêu demo: ít nhất 3 client chạy đồng thời; hai người đấu giá trong cùng phòng, một người thử vào phòng thứ hai; thể hiện thông báo 30 giây, gia hạn, mua ngay, kết quả và lịch sử sau khi khởi động lại server.

**Ràng buộc đã chốt:** phần socket và giao thức truyền nhận giữa client–server phải viết bằng C ở cả hai phía: kết nối, `recv/send`, ghép frame, encode/decode JSON và kiểm tra thông điệp. Giao diện được thiết kế tùy ý; có thể dùng C++/Qt Widgets gọi module mạng C qua header có `extern "C"`. Không dùng `QTcpSocket` để thay phần socket C. Server và bản nghiệm thu chạy trên Ubuntu WSL.

**Mô hình đã chốt:** người tổ chức (chủ phòng) chuẩn bị danh sách, thứ tự và lịch dự kiến của vật phẩm, rồi công bố trước cho khách xem và chuẩn bị ngân sách. Khách không tự đăng vật phẩm vào phòng. Tiền vẫn là VND mô phỏng; việc chuẩn bị ngân sách không đồng nghĩa bổ sung ví, nạp tiền hoặc thanh toán.

## 2. Phạm vi chốt cho bản hoàn chỉnh

| Nhóm | Chức năng bắt buộc |
| --- | --- |
| Tài khoản/phiên | Đăng ký, đăng nhập, đăng xuất, đổi mật khẩu, lưu mật khẩu dạng băm có salt; mỗi kết nối có danh tính đã xác thực; đóng kết nối giải phóng phiên và phòng đang tham gia. |
| Phòng | Người tổ chức tạo phòng nháp, chuẩn bị rồi công bố danh mục; tạo/liệt kê/xem chi tiết/vào/rời phòng; một tài khoản chỉ ở một phòng tại một thời điểm. Phòng nháp chỉ chủ phòng được xem. |
| Vật phẩm | Chỉ người tổ chức tạo/sửa/xóa và xếp thứ tự danh mục trong bản nháp; mỗi vật phẩm có mô tả, giá khởi điểm, giá mua ngay, thời điểm dự kiến, thời lượng. Sau công bố, khách xem trước đã bán/đang bán/sắp bán; bản cơ bản khóa thông tin và thứ tự đã công bố. |
| Khám phá | Khách đã đăng nhập xem danh mục và tìm theo tên/mô tả hoặc thời gian dự kiến trước khi vào phòng; chỉ trả phòng/vật phẩm đã công bố. Từ kết quả có thể vào phòng và nhận trạng thái hiện tại. |
| Đấu giá | Server quyết định mọi thay đổi giá; giá mới ≥ giá hiện tại + 10.000 VND; thông báo giá mới cho cả phòng; cảnh báo còn 30 giây; giá hợp lệ trong 30 giây cuối đặt lại hạn chót thành 30 giây; mua ngay được phép cả trước khi phiên bắt đầu và khi đang đấu giá. |
| Kết quả | Lưu người thắng, giá cuối, phương thức kết thúc và thời điểm; thống kê các phiên người dùng đã tham gia, đã thắng/thua và lịch sử đặt giá. |
| Vận hành | Log các thao tác quan trọng, lỗi và từ chối; GUI đủ để thao tác toàn bộ luồng demo. |

Tiền chỉ là đơn vị VND trong mô phỏng; project không xử lý thanh toán thực.

## 3. Kiến trúc và công nghệ đề xuất

```text
Qt Widgets GUI (C++)
        │ API C
Module client C (POSIX socket + JSON + frame)
        │ TCP / IPv4
C server (POSIX socket + poll)
        ├── trạng thái phòng/phiên trong bộ nhớ
        ├── bộ định thời đấu giá
        ├── SQLite (dữ liệu bền vững)
        └── log sự kiện
```

- **Server:** C11 trên Linux/WSL, socket không chặn, một event loop `poll()` quản lý socket lắng nghe và nhiều client. Mỗi client có buffer nhận và hàng đợi gửi; xử lý `recv/send` ngắn, `EAGAIN`, ngắt kết nối, frame sai và client chậm. Event loop duy nhất giúp tuần tự hóa hai lệnh đặt giá đến gần như đồng thời. Không giữ khóa CSDL hoặc thực hiện việc dài trong callback socket.
- **Client:** GUI C++/Qt Widgets gọi module C phụ trách POSIX socket, buffer, ghép frame, encode/decode JSON và request/event. Chạy xử lý mạng trong worker hoặc dùng thông báo fd sẵn sàng để không chặn GUI; không dùng `QTcpSocket`. Các màn hình gồm đăng nhập, danh mục/lịch đã công bố, phòng đấu giá, quản lý bản nháp và công bố dành cho người tổ chức, lịch sử. GUI hiển thị deadline từ server; đồng hồ client chỉ là hiển thị. Client CLI phase 1 đã dùng C; tách module này thành thư viện C có API dùng từ C++ khi tích hợp GUI.
- **Lưu trữ:** SQLite cho tài khoản, phòng, vật phẩm, giá đặt, kết quả và log. Mọi thay đổi đấu giá được ghi trong một transaction trước khi phát sự kiện. Bật `PRAGMA foreign_keys=ON` cho mỗi kết nối CSDL.
- **Build:** `Makefile` cho server, CMake cho Qt client. Có script chạy demo với một server và 3 client; README ghi phụ thuộc, cách build/chạy trên máy giảng viên.
- **Thư viện:** phase 1 dùng json-c và SQLite (phiên bản đã kiểm thử ghi trong README). Trước phase 2 chọn thư viện băm mật khẩu có salt được duy trì; không tự thiết kế thuật toán băm.

Tài liệu kỹ thuật tham khảo: [poll(2)](https://man7.org/linux/man-pages/man2/poll.2.html), [Qt Widgets](https://doc.qt.io/qt-6/qtwidgets-module.html), [SQLite transactions](https://www.sqlite.org/lang_transaction.html).

## 4. Giao thức truyền dòng

- Mỗi frame: `uint32` độ dài theo network byte order + nội dung UTF-8 JSON. Giới hạn ví dụ 64 KiB/frame; quá giới hạn thì báo lỗi và đóng kết nối.
- Mỗi request có `type`, `request_id`, `payload`; response trả đúng `request_id`, `ok/error_code`, `data`; sự kiện server đẩy dùng `event`, `room_id`, `item_id`, `version`.
- Lệnh dự kiến cho bản hoàn chỉnh: `REGISTER`, `LOGIN`, `LOGOUT`, `PASSWORD_CHANGE`, `ROOM_CREATE`, `ROOM_LIST`, `ROOM_DETAIL`, `ROOM_PUBLISH`, `ROOM_JOIN`, `ROOM_LEAVE`, `ROOM_SNAPSHOT`, `ITEM_CREATE`, `ITEM_UPDATE`, `ITEM_DELETE`, `ITEM_REORDER`, `ITEM_LIST`, `ITEM_SEARCH`, `BID`, `BUY_NOW`, `HISTORY_GET`. Phase 1 hiện chỉ hỗ trợ `PING`; các lệnh nghiệp vụ được triển khai ở phase 2–4.
- Sự kiện tối thiểu: `MEMBER_JOINED`, `MEMBER_LEFT`, `ITEM_STARTED`, `BID_ACCEPTED`, `TIME_WARNING`, `TIME_EXTENDED`, `ITEM_FINISHED`, `NEXT_ITEM_SCHEDULED`. Công bố danh mục là transaction; khách tra cứu được dữ liệu đã công bố ngay sau commit.
- Sau `ROOM_JOIN`, server gửi snapshot đầy đủ trước các sự kiện tiếp theo. Dùng `version` tăng dần theo phòng để client phát hiện sự kiện cũ/trùng và yêu cầu snapshot nếu cần.
- Viết test riêng cho 1 frame bị chia thành nhiều lần `recv`, nhiều frame gộp trong một lần `recv`, ghi từng phần, nội dung UTF-8, đóng kết nối giữa frame.

Ví dụ: `{"type":"BID","request_id":"b123","payload":{"item_id":42,"amount_vnd":150000}}`.

## 5. Mô hình dữ liệu và quyền

| Bảng | Trường chính |
| --- | --- |
| `users` | `id`, `username UNIQUE`, `password_hash`, `created_at` |
| `rooms` | `id`, `owner_user_id` (người tổ chức), `name`, `description`, `status` (`DRAFT/PUBLISHED/ACTIVE/FINISHED`), `published_at`, `created_at` |
| `items` | `id`, `room_id`, `title`, `description`, `starting_price_vnd`, `buy_now_price_vnd`, `planned_start_at`, `duration_sec`, `queue_position`, `status`, `deadline_at`, `winner_user_id`, `final_price_vnd`, `finish_method`, `finished_at` (người tổ chức lấy từ `rooms.owner_user_id`) |
| `bids` | `id`, `item_id`, `bidder_user_id`, `amount_vnd`, `created_at` |
| `participations` | `user_id`, `room_id`, `item_id`, `joined_at`, `left_at` (phục vụ thống kê) |
| `activity_logs` | `id`, `user_id`, `room_id`, `action`, `result`, `created_at`, `details` |

- Chỉ người đã đăng nhập được dùng API nghiệp vụ. Khách được xem danh mục đã công bố mà không cần `ROOM_JOIN`; vào phòng mới được bid/mua ngay. Việc xem trước không chiếm `active_room_id`.
- Chỉ chủ phòng được tạo/sửa/xóa/sắp thứ tự vật phẩm trong phòng `DRAFT`. Khách không có quyền này, kể cả khi đang ở phòng. Phòng nháp không xuất hiện trong danh sách/tìm kiếm của khách; truy vấn trực tiếp ID cũng phải kiểm tra quyền.
- Chỉ chủ phòng được `ROOM_PUBLISH`. Server kiểm tra danh mục không rỗng, thông tin/giá/thời lượng hợp lệ, thứ tự không trùng, lịch dự kiến chưa ở quá khứ và không giảm theo thứ tự hàng đợi. Công bố cả phòng trong một transaction và ghi `published_at`. Sau công bố không tạo/sửa/xóa/đổi thứ tự trong bản cơ bản, kể cả trước giờ mở, để khách chuẩn bị theo danh mục ổn định. Nếu sau này cần đổi lịch/hủy phải thiết kế luồng riêng, có log và thông báo.
- Chủ phòng không được đặt giá hoặc mua ngay vật phẩm trong phòng mình tổ chức. Server kiểm tra quyền từng request, không tin nút bị ẩn trên GUI. Một tài khoản có thể tổ chức phòng này và làm khách ở phòng khác.
- Một `user_id` chỉ có một `active_room_id` trong quản lý phiên server. `ROOM_JOIN` phòng khác bị từ chối cho tới khi rời phòng cũ. Nếu cho phép nhiều lần đăng nhập cùng tài khoản, cả hai kết nối dùng cùng ràng buộc này.
- Mật khẩu không xuất hiện trong log. Giá dùng số nguyên 64 bit, không dùng số thực; kiểm tra tràn số và giá không âm.

## 6. Quy tắc thời gian và hàng đợi

1. Mỗi phòng có một hàng đợi theo `queue_position`, tối đa một vật phẩm `LIVE`. Vật phẩm kế tiếp bắt đầu khi vật phẩm trước kết thúc và đã tới `planned_start_at`. Nếu phiên trước bị gia hạn, thời điểm bắt đầu của vật phẩm sau là **dự kiến**; kết quả tìm kiếm cần hiển thị rõ điều này.
   - Phòng đi qua `DRAFT → PUBLISHED → ACTIVE → FINISHED`. Bản nháp không tự chạy timer và không được mua ngay. Sau công bố, server tự mở vật phẩm đầu khi đến lịch; không phụ thuộc chủ phòng đang online. Phòng thành `ACTIVE` khi có vật phẩm bắt đầu; khi mọi vật phẩm đã có trạng thái kết thúc, phòng thành `FINISHED` (kể cả tất cả được mua ngay trước giờ mở).
2. Trạng thái vật phẩm: `QUEUED → LIVE → SOLD/UNSOLD`, `QUEUED → SOLD` khi mua ngay trước phiên, hoặc `QUEUED → CANCELLED`. Khi bắt đầu, giá hiện tại bằng giá khởi điểm và `deadline = now + duration_sec`.
3. Lệnh `BID` chỉ hợp lệ khi vật phẩm `LIVE`, người gửi đang ở phòng, không phải người tổ chức, `now < deadline`, và `amount ≥ current_price + 10000`. Bid đầu tiên cũng theo quy tắc này.
4. Khi còn ≤30 giây, gửi `TIME_WARNING` một lần cho hạn chót hiện hành. Nếu bid hợp lệ trong đoạn này, lưu bid rồi đặt `deadline = now + 30 giây` và gửi `TIME_EXTENDED`; mọi client lấy hạn chót mới từ server.
5. `BUY_NOW` hợp lệ khi phòng đã công bố, vật phẩm `QUEUED` hoặc `LIVE`, người mua ở phòng, không phải người tổ chức, và giá mua ngay vẫn lớn hơn giá hiện tại (với `QUEUED`, so với giá khởi điểm). Nó kết thúc vật phẩm ngay, ghi người mua/giá/phương thức/thời điểm và loại khỏi hàng đợi. Nếu A đang `LIVE` và B đang chờ được mua ngay, A tiếp tục, không mở thêm C; chỉ chọn vật phẩm kế tiếp khi phòng không có `LIVE` và lịch cho phép. Nếu bid đạt/vượt giá mua ngay thì vô hiệu hóa mua ngay. Giữ chức năng mua ngay trước giờ mở của plan cũ, nhưng chỉ sau khi công bố.
6. Event loop xử lý lệnh theo một thứ tự duy nhất. Trước mỗi lệnh liên quan đến vật phẩm, kiểm tra `now >= deadline` để kết thúc đúng hạn; bid hoặc mua ngay đến sau hạn bị từ chối. Timer phải hoạt động ngay cả khi không có request: tính timeout `poll()` theo mốc mở phiên/cảnh báo/kết thúc gần nhất. Dùng monotonic khi chạy, UTC khi lưu. Chính sách khôi phục nghiệp vụ phải chốt trước phase 3; hiện phase 1 mới kiểm thử độ bền dữ liệu nền tảng.

## 7. Thứ tự triển khai và mốc nghiệm thu

Ước lượng cho nhóm 3 người: 4–5 tuần. Nếu làm một mình, dùng thứ tự tương tự và dành khoảng 6–8 tuần. Mỗi mốc chỉ hoàn thành khi chạy được demo nhỏ tương ứng.

| Mốc | Công việc | Điều kiện hoàn thành |
| --- | --- | --- |
| 1. Nền tảng mạng (tuần 1) | Repo/build WSL, migration nền tảng SQLite, TCP server `poll()`, client CLI C, bộ ghép khung hai phía, mã lỗi, log. Schema nghiệp vụ thêm dần ở phase sau. | 3 client đồng thời; frame chia/gộp, UTF-8, client ngắt/chậm, restart và rollback CSDL; kiểm tra bộ nhớ. Đã có code và 14 test tích hợp đạt. |
| 2. Bản demo tài khoản, phòng và danh mục (tuần 2) | Đăng ký/đăng nhập/đăng xuất/đổi mật khẩu, phiên; tạo/liệt kê/xem/vào/rời phòng; chuẩn bị/sắp danh mục nháp và công bố; khách xem trước; snapshot và sự kiện thành viên; menu C giữ kết nối, thư viện mạng C dùng lại, migration v2. Phần danh mục cơ bản được đưa lên từ phase 3 theo phạm vi người dùng đã duyệt. | Demo 3 client không nhập JSON: công bố danh mục rồi khách xem/vào phòng; không vào 2 phòng; không sửa/xem nháp trái quyền; ngắt kết nối giải phóng phiên; restart giữ tài khoản/danh mục, xóa phiên; test hồi quy và bộ nhớ đạt. Chưa mở đấu giá tự động. |
| 3. Lõi đấu giá (tuần 3) | Lịch tự mở sau công bố, trạng thái, bid, mua ngay, timer 30 giây, sự kiện đấu giá, transaction kết quả và khôi phục nghiệp vụ; mở rộng schema v2 bằng migration tiếp theo. | Hai client thấy cùng giá; bid cạnh tranh có đúng một thứ tự; mua ngay/hết giờ không tạo kết quả trùng; restart đúng chính sách được chốt. |
| 4. GUI và tra cứu (tuần 4) | Qt gọi thư viện mạng C đã tách ở phase 2; màn quản lý/công bố, xem trước, đấu giá, tìm kiếm và lịch sử; mất kết nối và snapshot. | Đi hết kịch bản bằng GUI; toàn bộ kết nối/framing/JSON vẫn trong C; GUI không bị chặn khi chờ mạng. |
| 5. Ổn định và trình bày (cuối tuần 4–5) | Test tích hợp, restart server, sửa race/edge case, tài liệu, ảnh giao diện, video hoặc script demo. | Chạy lại toàn bộ checklist rubric trên máy sạch theo README. |

**Chia việc nếu 3 người:** (A) server socket/giao thức/phiên, (B) CSDL và nghiệp vụ đấu giá/timer, (C) Qt GUI. Cả nhóm cùng chốt `protocol.md` và schema trước khi làm song song; tích hợp mỗi ngày, không để tới cuối mới nối client với server.

## 8. Ưu tiên theo rubric

| Ưu tiên | Hạng mục | Cách chứng minh khi chấm |
| --- | --- | --- |
| P0 | Xử lý truyền dòng, socket server, tài khoản/phiên, phòng và phân quyền | Test frame chia/gộp, 3 client, demo đăng nhập/vào phòng thứ hai bị từ chối. |
| P0 | Danh mục do người tổ chức chuẩn bị/công bố, hàng đợi, bid, mua ngay, cảnh báo/gia hạn | Khách xem lịch trước khi vào phòng, không sửa được vật phẩm; demo hai người trả giá, cảnh báo/gia hạn và mua ngay đúng một lần. |
| P1 | Tìm kiếm, lịch sử/thống kê, log, GUI | Tìm vật phẩm rồi vào phòng; mở lịch sử và file log sau phiên. |
| P2 | Chức năng nâng cao | Chỉ thêm sau P0/P1: tự kết nối lại + đồng bộ snapshot; lọc tìm kiếm nâng cao; lịch sử giá dạng biểu đồ; chống request trùng; xuất thống kê CSV. Chọn 1–2 mục làm chắc. |

## 9. Checklist kiểm thử trước demo

- Hai client gửi bid cùng lúc: chỉ giá hợp lệ theo thứ tự xử lý được chấp nhận, không mất bid đã xác nhận.
- Bid bằng giá hiện tại, thấp hơn 10.000, đúng +10.000, số âm, số quá lớn; người ngoài phòng và chủ phòng tự bid.
- Chủ phòng tạo/sửa/xóa/sắp danh mục nháp; khách dùng trực tiếp API cũng bị từ chối. Khách không thấy nháp qua list/search/detail/item ID.
- Công bố danh mục thiếu thông tin hoặc lịch sai bị từ chối toàn bộ; sau công bố khách xem được giá/lịch/thứ tự mà chưa cần vào phòng. Xem trước nhiều phòng không vi phạm giới hạn một phòng đang tham gia.
- Khách không được mua ngay trong bản nháp. Danh mục đã công bố bị khóa chỉnh sửa; server tự mở đúng lịch dù chủ phòng offline và không có request.
- Một tài khoản cố vào hai phòng; đăng xuất hoặc mất mạng rồi vào lại.
- Còn 31 giây, đúng 30 giây, còn 1 giây, hết hạn; bid ở 30 giây cuối gia hạn đúng 30 giây và các GUI cùng thấy deadline mới.
- Mua ngay vật phẩm đã công bố đang chờ: bán và bỏ khỏi hàng đợi; không gián đoạn vật phẩm khác đang `LIVE`, không tạo hai `LIVE`. `BUY_NOW` đua với `BID` hoặc timer: chỉ một kết quả cuối cùng, một người thắng.
- Xóa item đã công bố/đang đấu giá bị từ chối; item tiếp theo bắt đầu đúng thứ tự và không sớm hơn lịch.
- Tìm theo từ khóa và khung giờ; kết quả sắp đấu giá ghi “thời điểm dự kiến”.
- Server/client khởi động lại: tài khoản, phòng, item, giá đặt và lịch sử vẫn đúng; không nhân đôi kết quả.
- Log có thời điểm, người dùng, phòng, lệnh, kết quả; không ghi mật khẩu hoặc dữ liệu nhạy cảm.

## 10. Quyết định đã chốt và điểm cần làm rõ ở phase sau

Đã chốt với người dùng: socket/giao thức hai phía viết bằng C; GUI tùy ý; chạy Ubuntu WSL; người tổ chức chuẩn bị và công bố danh mục trước cho khách xem.

Quy ước triển khai cho bản cơ bản: chủ phòng là người tổ chức; phòng nháp chỉ chủ phòng thấy; danh mục khóa sau công bố; khách đã đăng nhập được xem trước mà không vào phòng; vẫn giữ mua ngay sau công bố và trước giờ mở. Không bổ sung ví/nạp tiền. Các quy ước này có thể điều chỉnh trước phase 3 nếu yêu cầu chi tiết thay đổi.

Cần chốt trước phase liên quan:

1. Đã chốt cho phase 2: một tài khoản chỉ có một kết nối đăng nhập; lần đăng nhập thứ hai bị từ chối. Dùng libsodium Argon2id, một worker băm mật khẩu với tối đa 16 yêu cầu đang chờ/xử lý/hoàn tất; DB và phiên chỉ do event loop cập nhật. Chi tiết phạm vi phase 2 và demo: [docs/phase2.md](docs/phase2.md).
2. Trước phase 3: chi tiết lịch (hiện giữ `planned_start_at` và `duration_sec`), khoảng công bố trước giờ mở, chính sách khi restart đang đấu giá và cho phép sửa/hủy sau công bố nếu cần.
3. Trước khi ghi thống kê ở phase 3: “tham gia” tính theo xem/vào phòng hay đã bid; người chỉ xem có được tính thắng/thua không.

Không cần chờ các điểm này để chạy/kiểm thử phase 1. Payload, mã lỗi và migration nghiệp vụ phải được chốt trước khi code phase tương ứng.
