#include "client_net.h"
#include "protocol.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    ClientConnection net;
    long long user_id, selected_room, joined_room;
    int owner, draft, lost;
} Menu;
static volatile sig_atomic_t interrupted;
static void interrupt_menu(int sig) { (void)sig; interrupted = 1; }
static json_object *field(json_object *value, const char *name) {
    json_object *result = NULL; json_object_object_get_ex(value, name, &result); return result;
}
static long long integer(json_object *value, const char *name) { return json_object_get_int64(field(value, name)); }
static const char *string(json_object *value, const char *name) {
    const char *s = json_object_get_string(field(value, name)); return s ? s : "";
}
static void event_received(json_object *event, void *context) {
    (void)context;
    printf("\n[Thông báo] %s — %s, phòng %lld (version %lld)\n", string(event, "event"),
           string(event, "username"), integer(event, "room_id"), integer(event, "version"));
    fflush(stdout);
}
/* Poll stdin and the socket together, including when a prompt is idle. */
static int input(Menu *menu, const char *prompt, char *buffer, size_t capacity, int secret) {
    printf("%s", prompt); fflush(stdout);
    struct termios original, hidden;
    int terminal = secret && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original) == 0;
    if (terminal) { hidden = original; hidden.c_lflag &= (tcflag_t)~ECHO; tcsetattr(STDIN_FILENO, TCSANOW, &hidden); }
    int ok = 0;
    for (;;) {
        if (interrupted) break;
        /* A previous recv may already contain events after a response frame. */
        if (menu->net.received) {
            json_object *event;
            int rc;
            while ((rc = client_receive(&menu->net, 0, &event)) == 1) { event_received(event, menu); json_object_put(event); }
            if (rc < 0) { menu->lost = 1; break; }
        }
        struct pollfd fds[2] = {{.fd=STDIN_FILENO,.events=POLLIN}, {.fd=menu->net.fd,.events=POLLIN}};
        int ready = poll(fds, 2, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (fds[1].revents) {
            json_object *event;
            int rc;
            while ((rc = client_receive(&menu->net, 0, &event)) == 1) { event_received(event, menu); json_object_put(event); }
            if (rc < 0) { menu->lost = 1; break; }
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            if (!fgets(buffer, (int)capacity, stdin)) break;
            size_t length = strlen(buffer);
            if (length && buffer[length-1] == '\n') { buffer[length-1] = '\0'; ok = 1; }
            else if (feof(stdin)) ok = 1;
            else {
                int c; while ((c = getchar()) != '\n' && c != EOF) {}
                fprintf(stderr, "Dữ liệu quá dài, vui lòng nhập lại.\n"); break;
            }
            if (ok && strlen(buffer) && buffer[strlen(buffer)-1] == '\r') buffer[strlen(buffer)-1] = '\0';
            break;
        }
    }
    if (terminal) { tcsetattr(STDIN_FILENO, TCSANOW, &original); putchar('\n'); }
    return ok;
}
static int ask_text(Menu *menu, json_object *payload, const char *name, const char *prompt, int secret) {
    char buffer[1024];
    if (!input(menu, prompt, buffer, sizeof buffer, secret)) return 0;
    json_object_object_add(payload, name, json_object_new_string(buffer));
    memset(buffer, 0, sizeof buffer); return 1;
}
static int ask_number(Menu *menu, json_object *payload, const char *name, const char *prompt) {
    char buffer[64], *end;
    if (!input(menu, prompt, buffer, sizeof buffer, 0)) return 0;
    errno = 0; long long value = strtoll(buffer, &end, 10);
    if (errno || end == buffer || *end) { puts("Cần nhập số nguyên."); return 0; }
    json_object_object_add(payload, name, json_object_new_int64(value)); return 1;
}
static void display_items(json_object *items) {
    puts("Vật phẩm (thời gian bắt đầu là dự kiến):");
    for (size_t i = 0; i < json_object_array_length(items); i++) {
        json_object *item = json_object_array_get_idx(items, i);
        time_t start = (time_t)integer(item, "planned_start_at");
        struct tm tm; char stamp[32];
        gmtime_r(&start, &tm); strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M UTC", &tm);
        printf("  #%lld [thứ tự %lld] %s | %s\n    Khởi điểm: %lld VND; mua ngay: %lld VND; %s; %lld giây\n    %s\n",
               integer(item,"id"), integer(item,"queue_position"), string(item,"title"), string(item,"status"),
               integer(item,"starting_price_vnd"), integer(item,"buy_now_price_vnd"), stamp,
               integer(item,"duration_sec"), string(item,"description"));
    }
}
static void display(Menu *menu, const char *command, json_object *reply) {
    if (!json_object_get_boolean(field(reply, "ok"))) { printf("Thất bại: %s\n", string(reply,"error_code")); return; }
    json_object *data = field(reply,"data"); puts("Thành công.");
    if (!strcmp(command,"LOGIN")) menu->user_id = integer(data,"user_id");
    if (!strcmp(command,"LOGOUT")) { menu->user_id = menu->joined_room = menu->selected_room = 0; menu->owner = menu->draft = 0; }
    if (!strcmp(command,"ROOM_LEAVE")) menu->joined_room = 0;
    json_object *rooms = field(data,"rooms"), *room = field(data,"room");
    if (!strcmp(command,"ROOM_CREATE")) room = data;
    if (room) {
        menu->selected_room = integer(room,"id");
        menu->owner = integer(room,"owner_user_id") == menu->user_id;
        menu->draft = !strcmp(string(room,"status"),"DRAFT");
        printf("Phòng #%lld: %s | %s | người tổ chức #%lld\n%s\n", menu->selected_room,
               string(room,"name"), string(room,"status"), integer(room,"owner_user_id"), string(room,"description"));
        if (!strcmp(command,"ROOM_JOIN")) menu->joined_room = menu->selected_room;
    }
    if (rooms) {
        for (size_t i=0; i<json_object_array_length(rooms); i++) {
            json_object *r = json_object_array_get_idx(rooms,i);
            printf("  #%lld | %s | %s | người tổ chức #%lld\n", integer(r,"id"), string(r,"name"), string(r,"status"), integer(r,"owner_user_id"));
        }
        if (field(data,"next_offset")) printf("Trang tiếp theo: offset %lld\n",integer(data,"next_offset"));
    }
    if (field(data,"items")) display_items(field(data,"items"));
    if (field(data,"members")) {
        json_object *members=field(data,"members"); puts("Thành viên hiện tại:");
        for (size_t i=0;i<json_object_array_length(members);i++) {
            json_object *m=json_object_array_get_idx(members,i);
            printf("  #%lld %s\n",integer(m,"user_id"),string(m,"username"));
        }
    }
    if (field(data,"item_id")) printf("ID vật phẩm: %lld\n",integer(data,"item_id"));
    if (!strcmp(command,"REGISTER")) printf("ID tài khoản: %lld. Hãy đăng nhập.\n",integer(data,"user_id"));
}
static int item_fields(Menu *menu, json_object *payload) {
    if (!ask_text(menu,payload,"title","Tên vật phẩm: ",0) || !ask_text(menu,payload,"description","Mô tả: ",0) ||
        !ask_number(menu,payload,"starting_price_vnd","Giá khởi điểm (VND): ") ||
        !ask_number(menu,payload,"buy_now_price_vnd","Giá mua ngay (VND): ")) return 0;
    char buffer[64]; struct tm tm = {0}; int year,month,day,hour,minute,used=0;
    if (!input(menu,"Bắt đầu dự kiến GIỜ MÁY WSL (YYYY-MM-DD HH:MM): ",buffer,sizeof buffer,0)) return 0;
    if (strlen(buffer)!=16 || sscanf(buffer,"%4d-%2d-%2d %2d:%2d%n",&year,&month,&day,&hour,&minute,&used)!=5 || buffer[used] ||
        year<1970 || year>9999 || month<1 || month>12 || day<1 || day>31 || hour<0 || hour>23 || minute<0 || minute>59) {
        puts("Thời gian không hợp lệ."); return 0;
    }
    tm.tm_year=year-1900; tm.tm_mon=month-1; tm.tm_mday=day; tm.tm_hour=hour; tm.tm_min=minute; tm.tm_isdst=-1;
    time_t start=mktime(&tm);
    if (start==(time_t)-1 || tm.tm_year!=year-1900 || tm.tm_mon!=month-1 || tm.tm_mday!=day) { puts("Ngày không hợp lệ."); return 0; }
    json_object_object_add(payload,"planned_start_at",json_object_new_int64((long long)start));
    return ask_number(menu,payload,"duration_sec","Thời lượng (30–86400 giây): ");
}
static int menu_run(Menu *menu) {
    setvbuf(stdin,NULL,_IONBF,0);
    puts("ĐẤU GIÁ — PHASE 2: tài khoản, phòng và danh mục. Chưa mở đấu giá tự động.");
    for (;;) {
        if (interrupted) break;
        char choice[32]; const char *command=NULL;
        printf("\nTài khoản #%lld | đang xem phòng #%lld | tham gia phòng #%lld\n",menu->user_id,menu->selected_room,menu->joined_room);
        if (!menu->user_id) puts("1. Đăng ký   2. Đăng nhập   0. Thoát");
        else {
            puts("1. Đăng xuất  2. Đổi mật khẩu  3. Tạo phòng nháp  4. Danh sách phòng\n5. Xem/chọn phòng  6. Vào phòng  7. Rời phòng  13. Snapshot  14. Xem vật phẩm");
            if (menu->owner && menu->draft) puts("Danh mục phòng nháp đang chọn: 8. Thêm  9. Sửa  10. Xóa  11. Sắp thứ tự  12. Công bố");
            puts("0. Thoát");
        }
        if (!input(menu,"Chọn: ",choice,sizeof choice,0) || !strcmp(choice,"0")) break;
        json_object *p=json_object_new_object(); int valid=1;
        if (!menu->user_id) {
            if (!strcmp(choice,"1")) command="REGISTER";
            else if (!strcmp(choice,"2")) command="LOGIN";
            if (command) valid=ask_text(menu,p,"username","Tên đăng nhập (3–32 ký tự a-z, 0-9, _): ",0) && ask_text(menu,p,"password","Mật khẩu (8–128 byte): ",1);
        } else if (!strcmp(choice,"1")) command="LOGOUT";
        else if (!strcmp(choice,"2")) { command="PASSWORD_CHANGE"; valid=ask_text(menu,p,"old_password","Mật khẩu hiện tại: ",1) && ask_text(menu,p,"new_password","Mật khẩu mới: ",1); }
        else if (!strcmp(choice,"3")) { command="ROOM_CREATE"; valid=ask_text(menu,p,"name","Tên phòng: ",0) && ask_text(menu,p,"description","Mô tả: ",0); }
        else if (!strcmp(choice,"4")) { command="ROOM_LIST"; valid=ask_number(menu,p,"offset","Offset (trang đầu nhập 0): "); }
        else if (!strcmp(choice,"5") || !strcmp(choice,"6") || !strcmp(choice,"13") || !strcmp(choice,"14")) {
            command=!strcmp(choice,"5")?"ROOM_DETAIL":!strcmp(choice,"6")?"ROOM_JOIN":!strcmp(choice,"13")?"ROOM_SNAPSHOT":"ITEM_LIST";
            valid=ask_number(menu,p,"room_id","ID phòng: ");
        } else if (!strcmp(choice,"7")) command="ROOM_LEAVE";
        else if (menu->owner && menu->draft) {
            json_object_object_add(p,"room_id",json_object_new_int64(menu->selected_room));
            if (!strcmp(choice,"8")) { command="ITEM_CREATE"; valid=item_fields(menu,p); }
            else if (!strcmp(choice,"9")) { command="ITEM_UPDATE"; valid=ask_number(menu,p,"item_id","ID vật phẩm: ") && item_fields(menu,p); }
            else if (!strcmp(choice,"10")) { command="ITEM_DELETE"; valid=ask_number(menu,p,"item_id","ID vật phẩm: "); }
            else if (!strcmp(choice,"11")) {
                command="ITEM_REORDER"; char order[1024]={0};
                valid=input(menu,"Toàn bộ ID theo thứ tự mới, cách nhau bằng dấu cách: ",order,sizeof order,0);
                json_object *ids=json_object_new_array(); char *position=order;
                while (valid && *position) {
                    while (*position==' ') position++;
                    if (!*position) break;
                    char *end; errno=0; long long value=strtoll(position,&end,10);
                    if (errno || end==position || (*end && *end!=' ')) { valid=0; break; }
                    json_object_array_add(ids,json_object_new_int64(value)); position=end;
                }
                json_object_object_add(p,"item_ids",ids);
            } else if (!strcmp(choice,"12")) command="ROOM_PUBLISH";
        }
        if (command && valid) {
            json_object *reply=client_request(&menu->net,command,p,event_received,menu);
            if (!reply) { menu->lost=1; json_object_put(p); break; }
            display(menu,command,reply); json_object_put(reply);
        } else if (!command) puts("Lựa chọn không hợp lệ.");
        json_object_put(p);
        if (menu->lost) break;
    }
    if (menu->lost) fprintf(stderr,"Mất kết nối hoặc hết thời gian chờ. Hãy mở lại client và đăng nhập.\n");
    return menu->lost ? 1 : 0;
}
int main(int argc, char **argv) {
    if (argc!=3 && argc!=4) { fprintf(stderr,"usage: %s HOST_IPV4 PORT ['JSON']\nOmit JSON for interactive menu.\n",argv[0]); return 1; }
    char *end; errno=0; long port=strtol(argv[2],&end,10);
    if (errno || end==argv[2] || *end || port<1 || port>65535) return 1;
    Menu menu={0};
    struct sigaction action = {0};
    action.sa_handler = interrupt_menu;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
    if (client_connect(&menu.net,argv[1],(int)port)<0) { perror("connect"); return 1; }
    int result=1;
    if (argc==3) result=menu_run(&menu);
    else {
        json_object *request=protocol_parse((const unsigned char *)argv[3],strlen(argv[3])), *reply=NULL;
        if (request && client_send(&menu.net,request)==0 && client_receive(&menu.net,30000,&reply)==1) {
            puts(json_object_to_json_string_ext(reply,JSON_C_TO_STRING_PLAIN)); result=0;
        }
        json_object_put(request); json_object_put(reply);
        if (result) fprintf(stderr,"Invalid JSON, connection or frame transfer failed\n");
    }
    client_close(&menu.net); return result;
}
