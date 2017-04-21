#pragma once
//=====================================================================//
/*! @file
    @brief  SEEDA03 net クラス @n
			Copyright 2017 Kunihito Hiramatsu
    @author 平松邦仁 (hira@rvf-rc45.net)
*/
//=====================================================================//
#include <cstdio>
#include <cmath>
#include "rx64m_test/main.hpp"
#include "common/string_utils.hpp"
#include "chip/NTCTH.hpp"

#include "GR/core/ethernet.hpp"
#include "GR/core/ethernet_client.hpp"

extern "C" {
	void INT_Excep_ICU_GROUPAL1(void);
}

#define GET_DEBUG

namespace seeda {

	//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
	/*!
		@brief  net クラス
	*/
	//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
	class nets {

		typedef device::PORT<device::PORT7, device::bitpos::B0> LAN_RESN;
		typedef device::PORT<device::PORT7, device::bitpos::B3> LAN_PDN;

		net::ethernet			ethernet_;
		net::ethernet_server	server_;
		net::ethernet_client	client_;

		uint32_t		count_;

		enum class server_task {
			wait_client,
			main_loop,
			disconnect_delay,
			disconnect,
		};
		server_task		server_task_;

		enum class client_task {
			connection,
			main_loop,
			disconnect,
		};
		client_task		client_task_;

		typedef utils::line_manage<2048, 20> LINE_MAN;
		LINE_MAN	line_man_;

		uint32_t	disconnect_loop_;

		// サーミスタ定義
		// A/D: 12 bits, NT103, 分圧抵抗: 10K オーム、サーミスタ: ＶＣＣ側
		typedef chip::NTCTH<4095, chip::thermistor::NT103, 10000, true> THMISTER;
		THMISTER thmister_;

		static void dir_list_func_(const char* name, const FILINFO* fi, bool dir, void* option) {
			if(fi == nullptr) return;

			net::ethernet_client* cl = static_cast<net::ethernet_client*>(option);

			cl->print("<tr>");

			time_t t = utils::str::fatfs_time_to(fi->fdate, fi->ftime);
			struct tm *m = localtime(&t);
			if(dir) {
				cl->print("<td>-</td>");
			} else {
				char tmp[32];
				utils::format("<td>%9d</td>", tmp, sizeof(tmp)) % fi->fsize;
				cl->print(tmp);
			}
			{
				char tmp[64];
				utils::format("<td>%s %2d %4d</td>", tmp, sizeof(tmp)) 
					% get_mon(m->tm_mon)
					% static_cast<int>(m->tm_mday)
					% static_cast<int>(m->tm_year + 1900);
				cl->print(tmp);
				utils::format("<td>%02d:%02d</td>", tmp, sizeof(tmp)) 
					% static_cast<int>(m->tm_hour)
					% static_cast<int>(m->tm_min);
				cl->print(tmp);
				if(dir) {
					cl->print("<td>/");
				} else {
					cl->print("<td> ");
				}
				cl->print(name);
				cl->print("</td>");
				cl->println("</tr>");
			}
		}


		void send_info_(net::ethernet_client& client, int id, bool keep)
		{
			client << "HTTP/1.1 ";
			client << id << " OK";
			client.println();
			client.println("Server: seeda/rx64m");
			client.println("Content-Type: text/html");
			client.print("Connection: ");
			if(keep) client.println("keep-alive");
			else client.println("close");
			client.println();
		}


		void send_head_(net::ethernet_client& client, const char* title)
		{
			client.println("<head>");
			char tmp[256];
			utils::format("<title>SEEDA %s</title>", tmp, sizeof(tmp)) % title;
			client.println(tmp);
			client.println("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\">");
			client.println("<meta http-equiv=\"Pragma\" content=\"no-cache\">");
			client.println("<meta http-equiv=\"Cache-Control\" content=\"no-cache\">");
			client.println("<meta http-equiv=\"Expires\" content=\"0\">");
			client.println("</head>");
		}


		// クライアントからの応答を解析して終端（空行）があったら「true」
		int analize_request_(net::ethernet_client& client)
		{
			char tmp[1024];
			int len = client.read(tmp, sizeof(tmp));
			if(len <= 0) return -1;

			for(int i = 0; i < len; ++i) {
				char ch = tmp[i];
				if(ch == 0 || ch == 0x0d) continue;
				if(!line_man_.add(ch)) {
					utils::format("line_man: memory over\n");
					return -1;
				}
			}
			if(static_cast<size_t>(len) < sizeof(tmp)) {
				line_man_.set_term();
				if(!line_man_.empty()) {
#if 0
					for(uint32_t i = 0; i < line_man_.size(); ++i) {
						const char* p = line_man_[i];
						utils::format("%s\n") % p;
					}
#endif
					for(uint32_t i = 0; i < line_man_.size(); ++i) {
						const char* p = line_man_[i];
						if(p[0] == 0) {  // 応答の終端！（空行）
							return i;
						}
					}
				}
			}
			return -1;
		}


		void do_cgi_(net::ethernet_client& client, const char* path, int pos)
		{
			utils::format("CGI: '%s'\n") % path;

			int len = 0;
			for(int i = 0; i < static_cast<int>(line_man_.size()); ++i) {
				const char* p = line_man_[i];
				static const char* key = { "Content-Length: " };
				if(strncmp(p, key, strlen(key)) == 0) {
					utils::input("%d", p + strlen(key)) % len;
					break;
				}
			}

			int lines = static_cast<int>(line_man_.size());
			++pos;
			char body[256];
			if(pos >= lines) {
				utils::format("CGI No Body\n");
				return;
			} else {
				utils::str::conv_html_amp(line_man_[pos], body);
//				utils::format("CGI Body: '%s'\n") % body;
			}

			if(strcmp(path, "/cgi/set_rtc.cgi") == 0) {
				typedef utils::parse_cgi_post<256, 2> CGI_RTC;
				CGI_RTC cgi;
				cgi.parse(body);
				const char* date = nullptr;
				const char* time = nullptr;
				for(uint32_t i = 0; i < cgi.size(); ++i) {
					const auto& t = cgi.get_unit(i);
					if(strcmp(t.key, "date") == 0) {
						date = t.val;
					} else if(strcmp(t.key, "time") == 0) {
						time = t.val;
					}
				}
				if(date != nullptr && time != nullptr) {
					time_t t = seeda::make_time(date, time);
					if(t != 0) {
						seeda::set_time(t);
					}
				}
//				cgi.list();
			} else if(strcmp(path, "/cgi/set_level.cgi") == 0) {
				typedef utils::parse_cgi_post<256, 8> CGI_ADC;
				CGI_ADC cgi;
				cgi.parse(body);
				for(uint32_t i = 0; i < cgi.size(); ++i) {
					const auto& t = cgi.get_unit(i);
					int ch;
					if((utils::input("level_ch%d", t.key) % ch).status()) {
						sample_t samp = get_sample(ch);
						int v;
						if((utils::input("%d", t.val) % v).status()) {
							if(v >= 0 && v <= 65535) {
								samp.limit_level_ = v;
								set_sample(ch, samp);
							}
						}
					}
				}
//				cgi.list();
			}
		}


		// POST などの空行以下のデータ列を受け取る
		bool recv_data_(net::ethernet_client& client, char* recv, uint32_t max)
		{
			int len = client.read(recv, max);
			if(len <= 0) return false;

			return true;
		}


		void render_404_(net::ethernet_client& client, const char* msg)
		{
			send_info_(client, 404, false);
		}


		void render_null_(net::ethernet_client& client, const char* title = nullptr)
		{
			send_info_(client, 200, false);

			client.println("<!DOCTYPE HTML>");
			client.println("<html>");
			send_head_(client, "NULL");


			if(title != nullptr) {
				client.println(title);
			}
			client.println("</html>");
		}


		void render_root_(net::ethernet_client& client)
		{
			send_info_(client, 200, false);

			client.println("<!DOCTYPE HTML>");
			client.println("<html>");
			send_head_(client, "Root");

			client.println("</html>");
#if 0
			client.println("<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\" \"http://www.w3.org/TR/html4/loose.dtd\">");
			client.println("<html>");
			client.println("<head>");
			client.println("<title>SEEDA Settings</title>");
			client.println("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\">");
			client.println("<meta http-equiv=\"Pragma\" content=\"no-cache\">");
			client.println("<meta http-equiv=\"Cache-Control\" content=\"no-cache\">");
			client.println("<meta http-equiv=\"Expires\" content=\"0\">");
			client.println("<script src=\"seeda/OnceOnly.html\"></script>");
			client.println("</head>");
			client.println("<FRAMESET cols=\"223,*\" frameborder=\"NO\" border=\"0\">");
			client.println("<FRAMESET rows=\"137,*\" frameborder=\"NO\" border=\"0\">");
			client.println("<FRAME name=\"ttl\" scrolling=\"NO\" noresize src=\"seeda/title.png\">");
			client.println("<FRAME name=\"men\" scrolling=\"AUTO\" noresize src=\"seeda/men.html\">");
			client.println("</FRAMESET>");
			client.println("<FRAME name=\"frm\" src=\"seeda/frmtop.html\">");
			client.println("<NOFRAMES>");
			client.println("<p>You cann't access this page without frame brouser.</p>");
			client.println("</NOFRAMES>");
			client.println("</FRAMESET>");
			client.println("</html>");
#endif
		}


		// 設定画面
		void render_setup_(net::ethernet_client& client)
		{
			send_info_(client, 200, false);

			client.println("<!DOCTYPE HTML>");
			client.println("<html>");
			send_head_(client, "Setup");

			{  // ビルドバージョン表示
				char tmp[128];
				utils::format("Seeda03 Build: %u Version %d.%02d", tmp, sizeof(tmp)) % build_id_
					% (seeda_version_ / 100) % (seeda_version_ % 100);
				client.println(tmp);
				client.println("<br/>");
			}

			{  // 時間表示
				char tmp[128];
				time_t t = get_time();
				disp_time(t, tmp, sizeof(tmp));
				client.print(tmp);
				client.println("<br/>");
				client.println("<hr align=\"left\" width=\"400\" size=\"3\" />");
			}

			client.println("<input type=\"button\" onclick=\"location.href='/setup'\" value=\"リロード\" />");
			client.println("<hr align=\"left\" width=\"400\" size=\"3\" />");

			client.println("<input type=\"button\" onclick=\"location.href='/'\" value=\"データ表示\" />");
			client.println("<hr align=\"left\" width=\"400\" size=\"3\" />");

			{  // 内臓 A/D 表示
				char tmp[128];
				float v = static_cast<float>(get_adc(5)) / 4095.0f * 3.3f;
				utils::format("バックアップ電池電圧： %4.2f [V]", tmp, sizeof(tmp)) % v;
				client.println(tmp);
				client.println("<br>");
				client.println("<hr align=\"left\" width=\"400\" size=\"3\" />");
			}

//			client.println("<font size=\"4\">");
//			client.println("</font>");

			// RTC 設定 /// client.println("<input type=\"reset\" value=\"取消\">");
			{
				client.println("<form method=\"POST\" action=\"/cgi/set_rtc.cgi\">");
				auto t = get_time();
				struct tm *m = localtime(&t);
				char tmp[256];
				utils::format("<div>年月日(yyyy/mm/dd)：<input type=\"text\" name=\"date\" size=\"10\" value=\"%d/%d/%d\" /></div>", tmp, sizeof(tmp))
					% static_cast<int>(m->tm_year + 1900) % static_cast<int>(m->tm_mon + 1) % static_cast<int>(m->tm_mday);
				client.println(tmp);
				utils::format("<div>時間　(hh:mm[:ss])：<input type=\"text\" name=\"time\" size=\"8\" value=\"%d:%d\" /></div>", tmp, sizeof(tmp))
					% static_cast<int>(m->tm_hour) % static_cast<int>(m->tm_min);
				client.println(tmp);
				client.println("<input type=\"submit\" value=\"ＲＴＣ設定\" />");
				client.println("</form>");
				client.println("<hr align=\"left\" width=\"400\" size=\"3\" />");
			}

#if 0
			// サンプリング周期設定
			client.println("<form method=\"POST\" action=\"/cgi/set_rate.cgi\">");
			client.println("<p>Ａ／Ｄ変換サンプリング・レート<br>");
			client.println("<input type=\"radio\" name=\"rate\" value=\"_10ms\" />１０［ｍｓ］"); 
			client.println("<input type=\"radio\" name=\"rate\" value=\"_01ms\" />　１［ｍｓ］"); 
			client.println("</p>");
			client.println("<input type=\"submit\" value=\"レート設定\" />");
			client.println("</form>");
			client.println("<br>");
#endif

			// 閾値設定
			{
				client.println("<form method=\"POST\" action=\"/cgi/set_level.cgi\">");
				static const char* ch16[] = { "０", "１", "２", "３", "４", "５", "６", "７" };
				for (int ch = 0; ch < 8; ++ch) {
					const auto& t = get_sample(ch);
					char tmp[256];
					utils::format("<div>チャネル%s：<input type=\"text\" name=\"level_ch%d\" size=\"6\" value=\"%d\"  /></div>", tmp, sizeof(tmp))
						% ch16[ch] % ch % static_cast<int>(t.limit_level_);
					client.println(tmp);
				}
				client.println("<input type=\"submit\" value=\"Ａ／Ｄ変換閾値設定\" />");
				client.println("</form>");

				client.println("<hr align=\"left\" width=\"400\" size=\"3\" />");
			}

			// クライアント
			{
			}

			// ＳＤカード操作画面へのボタン
			client.println("<input type=\"button\" onclick=\"location.href='/files'\" value=\"ＳＤカード\" />");
			client.println("<hr align=\"left\" width=\"400\" size=\"3\" />");

			client.println("</html>");
		}


		void render_files_(net::ethernet_client& client)
		{
			send_info_(client, 200, false);

			client.println("<!DOCTYPE HTML>");
			client.println("<html>");
			send_head_(client, "SD Files");

			client.println("<style type=\"text/css\">");
			client.println(".table3 {");
			client.println("  border-collapse: collapse;");
			client.println("  width: 500px;");
			client.println("}");
			client.println(".table3 th {");
			client.println("  background-color: #cccccc;");
			client.println("}");
			client.println("</style>");
			client.println("<table class=\"table3\" border=1>");
			client.println("<tr><th>Size</th><th>Date</th><th>Time</th><th>Name</th></tr>");
			at_sdc().dir_loop("", dir_list_func_, true, &client);
			client.println("</table>");

			client.println("<br>");
			client.println("<hr align=\"left\" width=\"600\" size=\"3\" />");
			client.println("<input type=\"button\" onclick=\"location.href='/setup'\" value=\"設定\" />");

			client.println("</html>");
		}


		void render_data_(net::ethernet_client& client)
		{
			send_info_(client, 200, false);
			// client.println("Refresh: 5");  // refresh the page automatically every 5 sec

			client.println("<!DOCTYPE HTML>");
			client.println("<html>");
			send_head_(client, "Data");

			client.println("<font size=\"4\">");
			{  // コネクション回数表示
				char tmp[128];
				utils::format("Conection: %d", tmp, sizeof(tmp)) % count_;
				client.print(tmp);
				client.println("<br/>");
			}
			{  // 時間表示
				char tmp[128];
				time_t t = get_time();
				disp_time(t, tmp, sizeof(tmp));
				client.print(tmp);
				client.println("<br/>");
			}

			client << "サンプリング周期： 1[ms]\n";
			client << "<br/>\n";

			client.println("<br/>");
			client.println("</font>");

			{  // 内臓 A/D 表示（湿度、温度）
				char tmp[64];
				auto v = get_adc(6);
				utils::format("温度： %5.2f [度]", tmp, sizeof(tmp)) % thmister_(v);
				client.println(tmp);
				client.println("<hr align=\"left\" width=\"600\" size=\"3\" />");
			}

			client << "<style type=\"text/css\">\n";
			client << ".table5 {\n";
			client << "  border-collapse: collapse;\n";
			client << "  width: 500px;\n";
			client << "}\n";
			client << ".table5 th {\n";
			client << "  background-color: #cccccc;\n";
			client << "}\n";
			client << ".table5 td {\n";
			client << "  text-align: center;\n";
			client << "}\n";
			client << "</style>\n";

			client << "<table class=\"table5\" border=1>\n";
			client << " <tr><th>チャネル</th><th>最小値</th><th>最大値</th><th>平均</th>"
					  "<th>基準値</th><th>カウント</th><th>Median</th></tr>\n";
			for (int ch = 0; ch < 8; ++ch) {
				const auto& t = get_sample(ch);
				client << " <tr>";
				char tmp[128];
				utils::format("<td>%d</td>", tmp, sizeof(tmp)) % ch;
				client << tmp;
///				utils::format("<td>%d</td>", tmp, sizeof(tmp)) % static_cast<uint32_t>(t.value_);
///				client << tmp;
				utils::format("<td>%d</td>", tmp, sizeof(tmp)) % static_cast<uint32_t>(t.min_);
				client << tmp;
				utils::format("<td>%d</td>", tmp, sizeof(tmp)) % static_cast<uint32_t>(t.max_);
				client << tmp;
				utils::format("<td>%d</td>", tmp, sizeof(tmp)) % static_cast<uint32_t>(t.average_);
				client << tmp;
				utils::format("<td>%d</td>", tmp, sizeof(tmp)) % static_cast<uint32_t>(t.limit_level_);
				client << tmp;
				utils::format("<td>%d</td>", tmp, sizeof(tmp)) % static_cast<uint32_t>(t.limit_up_);
				client << tmp;
				utils::format("<td>%d</td>", tmp, sizeof(tmp)) % static_cast<uint32_t>(t.median_);
				client << tmp;
				client << "</tr>\n";
			}
			client << "</table>\n";

			client.println("<br>");

			client.println("<hr align=\"left\" width=\"600\" size=\"3\" />");
			client.println("<input type=\"button\" onclick=\"location.href='/setup'\" value=\"設定\" />");

			client.println("</html>");
		}


		void send_file_(net::ethernet_client& client, const char* path)
		{
			FILE* fp = fopen(path, "rb");
			if(fp != nullptr) {
				client.println("HTTP/1.1 200 OK");
				client.print("Content-Type: ");
				const char* ext = strrchr(path, '.');
				if(ext != nullptr) {
					++ext;
					if(strcmp(ext, "png") == 0 || strcmp(ext, "jpeg") == 0 || strcmp(ext, "jpg") == 0) {
						client.print("image/");
					} else {
						client.print("text/");
					}
					client.println(ext);
				} else {
					client.println("text/plain");
				}
				client.println("Connection: close");
				client.println();
				uint8_t tmp[512];
				uint32_t total = 0;
				uint32_t len;
				while((len = fread(tmp, 1, sizeof(tmp), fp)) == sizeof(tmp)) {
					client.write(tmp, sizeof(tmp));
					total += len;
				}
				if(len > 0) {
					client.write(tmp, len);
					total += len;
				}
				fclose(fp);
			} else {
				render_null_(client, path);
			}
		}


		void get_path_(const char* src, char* dst) {
			int n = 0;
			char ch;
			while((ch = src[n]) != 0) {
				if(ch == ' ') break;
				dst[n] = ch;
				++n;
			}
			dst[n] = 0;
		}


		void service_client_()
		{
			switch(client_task_) {
			case client_task::connection:
				if(client_.connected()) {
					client_task_ = client_task::main_loop;
					break;
				}

				if(client_.connect(net::ip_address(192, 168, 3, 4), 3000, 5) == 1) {
					utils::format("Conected\n");
					client_task_ = client_task::main_loop;
				}
				break;

			case client_task::main_loop:
				if(!client_.connected()) {
					client_task_ = client_task::disconnect;
					break;
				}

//				client_.print("SEEDA03");

				break;
			
			case client_task::disconnect:
				client_.stop();
				utils::format("client disconnected\n");
				client_task_ = client_task::connection;
				break;
			}


#if 0
					static int nnn = 0;
	 				if(nnn >= 100) {
						utils::format(".\n");
						client_.print("SEEDA03");
						nnn = 0;
					} else {
						++nnn;
					}
#endif

		}

	public:
		//-----------------------------------------------------------------//
		/*!
			@brief  コンストラクタ
		*/
		//-----------------------------------------------------------------//
		nets() : server_(), count_(0), server_task_(server_task::wait_client),
			client_task_(client_task::connection),
			line_man_(0x0a), disconnect_loop_(0) { }


		//-----------------------------------------------------------------//
		/*!
			@brief  初期化
		*/
		//-----------------------------------------------------------------//
		void init()
		{
			// LAN initialize (PHY reset, PHY POWER-DOWN
			LAN_PDN::DIR = 1;  // output;
			LAN_PDN::P = 1;    // Not Power Down Mode..
			LAN_RESN::DIR = 1; // output;
			LAN_RESN::P = 0;
			utils::delay::milli_second(200); /// reset rise time
			LAN_RESN::P = 1;

#ifdef SEEDA
			device::power_cfg::turn(device::peripheral::ETHERCA);
			device::port_map::turn(device::peripheral::ETHERCA);
#else
			device::power_cfg::turn(device::peripheral::ETHERC0);
			device::port_map::turn(device::peripheral::ETHERC0);
#endif
			set_interrupt_task(INT_Excep_ICU_GROUPAL1, static_cast<uint32_t>(device::icu_t::VECTOR::GROUPAL1));

			ethernet_.maininit();

			static const uint8_t mac[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

			bool develope = true;
#ifdef SEEDA
			if(get_switch() != 0) {
				develope = false;
			}
#endif

			if(develope) {
				if(ethernet_.begin(mac) == 0) {
					utils::format("Ethernet Fail: begin (DHCP)...\n");

					utils::format("SetIP: ");
					net::ip_address ipa(192, 168, 3, 20);
					ethernet_.begin(mac, ipa);
				} else {
					utils::format("DHCP: ");
				}
			} else {
				utils::format("SetIP: ");
				net::ip_address ipa(192, 168, 1, 10);
				ethernet_.begin(mac, ipa);
			}
			ethernet_.localIP().print();
			utils::format("\n");

			server_.begin(80);
			utils::format("Start server: ");
			ethernet_.localIP().print();
			utils::format("  port(%d)\n") % static_cast<int>(server_.get_port());

#if 0
			client_.begin(3000);
			utils::format("Start client: ");
			ethernet_.localIP().print();
			utils::format("  port(%d)\n") % static_cast<int>(client_.get_port());
#endif
		}


		//-----------------------------------------------------------------//
		/*!
			@brief  タイトル表示（ネット関係）
		*/
		//-----------------------------------------------------------------//
		void title()
		{
		}


		//-----------------------------------------------------------------//
		/*!
			@brief  サービス
		*/
		//-----------------------------------------------------------------//
		void service()
		{
			ethernet_.mainloop();

///			service_client_();

			net::ethernet_client client = server_.available();

			switch(server_task_) {

			case server_task::wait_client:
				if(client) {
					utils::format("new client\n");
					++count_;
					line_man_.clear();
					server_task_ = server_task::main_loop;
				}
				break;

			case server_task::main_loop:
				if(client.connected()) {

					if(client.available() == 0) {
///						utils::format("client not available\n");
						break;
					}
					auto pos = analize_request_(client);
					if(pos > 0) {
						char path[256];
						path[0] = 0;
						if(!line_man_.empty()) {
							const auto& t = line_man_[0];
							if(strncmp(t, "GET ", 4) == 0) {
								get_path_(t + 4, path);
							} else if(strncmp(t, "POST ", 5) == 0) {
								get_path_(t + 5, path);
							}
						} else {
							utils::format("fail GET/POST data section\n");
							break;
						}
						if(strcmp(path, "/") == 0) {
///							render_root_(client);
							render_data_(client);
						} else if(strncmp(path, "/cgi/", 5) == 0) {
							do_cgi_(client, path, pos);
							render_setup_(client);
						} else if(strcmp(path, "/data") == 0) {
							render_data_(client);
						} else if(strcmp(path, "/setup") == 0) {
							render_setup_(client);
						} else if(strcmp(path, "/files") == 0) {
							render_files_(client);
						} else if(strncmp(path, "/seeda/", 7) == 0) {
							send_file_(client, path);
						} else {
							char tmp[256];
							utils::format("Invalid path: '%s'", tmp, sizeof(tmp)) % path;
							render_null_(client, tmp);
						}
						server_task_ = server_task::disconnect_delay;
						disconnect_loop_ = 5;
					}
				} else {
					server_task_ = server_task::disconnect_delay;
					disconnect_loop_ = 5;
				}
				break;

			case server_task::disconnect_delay:
				if(disconnect_loop_ > 0) {
					--disconnect_loop_;
				} else {
					server_task_ = server_task::disconnect;
				}
				break;

			case server_task::disconnect:
			default:
				client.stop();
				utils::format("client disconnected\n");
				server_task_ = server_task::wait_client;
				break;
			}
		}


		//-----------------------------------------------------------------//
		/*!
			@brief  PHY リセット信号制御
			@param[in]	cmd		コマンド入力インスタンス
		*/
		//-----------------------------------------------------------------//
		bool reset_signal(CMD cmd)
		{
			uint8_t cmdn = cmd.get_words();
			bool f = false;
			if(cmdn == 1) {
				bool v = LAN_RESN::P();
				utils::format("LAN-RESN: %d\n") % static_cast<int>(v);
				return true;
			} else if(cmdn > 1) {
				char tmp[16];
				if(cmd.get_word(1, sizeof(tmp), tmp)) {
					// Reset signal
					if(strcmp(tmp, "0") == 0) {
						device::PORT7::PODR.B0 = 0;
						f = true;
					} else if(strcmp(tmp, "1") == 0) {
						device::PORT7::PODR.B0 = 1;
						f = true;
					} else {
						utils::format("reset param error: '%s'\n") % tmp;
					}
				}
			}
			return f;
		}
	};
}