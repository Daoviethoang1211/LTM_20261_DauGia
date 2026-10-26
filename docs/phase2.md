# Phase 2 — phạm vi đã duyệt và hướng dẫn nghiệm thu

Mục tiêu: demo chức năng cơ bản bằng menu C, không nhập JSON. Ngoài tài khoản và phòng, đưa quản lý/công bố danh mục cơ bản từ phase 3 lên phase 2 để người tổ chức và hai khách có thể thao tác cùng nhau.

## Chức năng

- Đăng ký, đăng nhập, đăng xuất, đổi mật khẩu; username 3–32 byte `[a-z0-9_]`, mật khẩu 8–128 byte không chứa ký tự điều khiển.
- Argon2id qua libsodium, salt ngẫu nhiên, không lưu/log mật khẩu thô. Một worker băm có hàng đợi hữu hạn; tối đa 16 yêu cầu auth chưa được thu kết quả. Trả `SERVER_BUSY` khi đầy.
- Mỗi tài khoản có một kết nối đăng nhập; lần đăng nhập thứ hai bị từ chối. Kết quả worker gắn generation của kết nối, không áp dụng vào fd/slot đã tái sử dụng. Chỉ event loop truy cập SQLite và cập nhật phiên.
- Người tổ chức tạo phòng DRAFT, thêm/sửa/xóa/sắp thứ tự vật phẩm, tối đa 30 vật phẩm/phòng để snapshot luôn nằm trong frame 64 KiB. Phòng nháp chỉ chủ phòng thấy.
- Công bố khi danh mục không rỗng, lịch ở tương lai và không giảm theo thứ tự. Danh mục khóa sau công bố; khách đã đăng nhập xem trước mà không cần vào phòng.
- Vào/rời một phòng; snapshot có phòng, vật phẩm, thành viên và version; người đang ở phòng nhận MEMBER_JOINED/MEMBER_LEFT.
- SQLite schema v2 tự nâng cấp từ v1. Tài khoản, phòng, vật phẩm và version bền vững; phiên/thành viên chỉ ở RAM và phải đăng nhập lại sau restart.
- Client C có menu và thư viện `build/libauction_client.a`; API C ở `src/client_net.h`, gọi được từ C++. Chế độ gửi một JSON request của phase 1 vẫn được giữ.

Chưa có bid, mua ngay, tự mở phiên theo lịch, đồng hồ đếm ngược, kết quả, tìm kiếm, lịch sử hoặc GUI Qt. Phòng PUBLISHED vẫn chờ phase 3, kể cả khi đến giờ dự kiến. Các field đấu giá sẽ được bổ sung bằng migration tiếp theo.

## Demo bằng 4 terminal Ubuntu

Terminal server:

```bash
cd /mnt/c/Users/ADMIN/Downloads/LTM
make
make run PORT=5555 DB=var/phase2-demo.db
```

Ba terminal còn lại, mỗi terminal chạy:

```bash
cd /mnt/c/Users/ADMIN/Downloads/LTM
make client PORT=5555
```

1. Mỗi client chọn 1 đăng ký với username khác nhau, rồi chọn 2 đăng nhập. Chọn mật khẩu riêng để nhập tại prompt, không đặt trong lệnh shell.
2. Client người tổ chức: chọn 3 tạo phòng nháp; phòng vừa tạo được chọn tự động. Chọn 8 thêm vật phẩm; nhập giờ máy WSL (`date` để xem múi giờ), nên chọn ngày mai. Giá là số nguyên VND; giá mua ngay phải lớn hơn giá khởi điểm, thời lượng 30–86400 giây.
3. Chọn 14 và nhập ID phòng để xem vật phẩm/ID; chọn 9 sửa, 10 xóa hoặc 11 nhập toàn bộ ID theo thứ tự mới. Xếp lịch không giảm theo thứ tự, rồi chọn 12 công bố. Tạo/công bố thêm phòng thứ hai để kiểm thử chuyển phòng.
4. Trước công bố: hai khách chọn 4, offset 0; không thấy phòng nháp. Thử chọn 5 với ID nháp cũng bị NOT_FOUND.
5. Sau công bố: khách chọn 4 rồi 5 để xem phòng, danh mục, giá và lịch. Xem trước không chiếm phòng đang tham gia.
6. Hai khách chọn 6 vào cùng phòng. Khách vào trước nhận thông báo MEMBER_JOINED; chọn 13 để xem snapshot hai thành viên.
7. Một khách chọn 6 vào phòng thứ hai khi chưa rời: ALREADY_IN_ROOM. Chọn 7 rời rồi chọn 6 vào lại sẽ thành công. Khách còn lại nhận MEMBER_LEFT.
8. Thử đăng nhập tài khoản đang dùng từ client khác: ALREADY_LOGGED_IN. Đăng xuất hoặc đóng client cũ rồi đăng nhập lại. Đổi mật khẩu bằng lựa chọn 2 sau khi đã đăng nhập.
9. Ctrl+C server, chạy lại cùng DB, mở lại các client và đăng nhập. Phòng/danh mục vẫn còn; danh sách thành viên bắt đầu rỗng.

Menu 8–12 chỉ hiển thị khi phòng đang chọn thuộc người dùng và còn DRAFT. Chọn 5 xem/chọn một phòng của mình để quản lý tiếp; server vẫn kiểm tra quyền ở mọi request. Mật khẩu không hiển thị khi nhập từ terminal. Mất kết nối không tự retry, cần mở lại client.

## Kiểm thử

```bash
make test
bash scripts/check-memory.sh
```

Bộ test dùng server/cổng/CSDL tạm, gồm hồi quy phase 1, API phase 2, luồng menu C, cạnh tranh đăng nhập, nghẽn hàng đợi auth, stale auth khi tái sử dụng kết nối, quyền bản nháp, khóa sau công bố, thứ tự/snapshot/sự kiện, transaction rollback và migration v1→v2. Script bộ nhớ build với ASan/UBSan, chạy test rồi khôi phục build thường khi đạt.
