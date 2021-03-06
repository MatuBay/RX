//=====================================================================//
/*! @file
    @brief  EFO (RX64M) メイン
	@copyright Copyright 2017 Kunihito Hiramatsu All Right Reserved.
    @author 平松邦仁 (hira@rvf-rc45.net)
*/
//=====================================================================//
#include "common/renesas.hpp"
#include "common/cmt_io.hpp"
#include "common/tpu_io.hpp"
#include "common/fifo.hpp"
#include "common/sci_io.hpp"
#include "common/irq_man.hpp"
#include "chip/LTC2348_16a.hpp"

#if 0
・デバッグ用LED出力 ---> PJ5(11)
・デバッグ用シリアルＲＸＤ  --->  P21(36) / RXD0
・デバッグ用シリアルＴＸＤ  --->  P20(37) / TXD0
・変換トリガー閾値（コンパレーター入力+へ)   ---> P03(4) / DA0
・変換トリガー入力（コンパレーター出力から） ---> PF5(9) / IRQ4
#endif

// #define UART_DEBUG

namespace {

	static const int main_version_ = 41;
	static const uint32_t build_id_ = B_ID;

	enum class CMD : uint8_t {
		START = 0x01,
		END   = 0x03,
		MODE_SELECT = 0x11,
		MODE_SENSE  = 0x12,
		VER   = 0x21,

		ASC_CNVTEST = 'T',
		ASC_CNVTESTS = 't',
	};

	enum class TASK : uint8_t {
		STATE,	// コマンド待ち
		LENGTH,	// 長さ取得
		VOLT,	// トリガー電圧取得
	};

	enum class SEND_TASK : uint8_t {
		READY,
		SINGLE,
		MULTI,
		ASCII,		///< T コマンド（１０２４サンプルテスト）
		ASCII2,		///< t コマンド（１サンプルテスト）
	};

	typedef device::PORT<device::PORTJ, device::bitpos::B5> LED;

	typedef device::cmt_io<device::CMT0, utils::null_task> CMT;
	CMT		cmt_;

	// LTC2348-16 A/D 制御ポート定義
	// LTC2348ILX-16 (38:BUSY) ---> P16(40) / IRQ6
	// LTC2348ILX-16 (24:CNV)  ---> PB3(82) / MTIOC0A
	// LTC2348ILX-16 (37:SDI)  ---> P50(56) / SMOSI2
	// LTC2348ILX-16 (25:SDO0) ---> P52(54) / SMISO2
	// LTC2348ILX-16 (26:SDO1) ---> P25(32) / SMISO3
	// LTC2348ILX-16 (29:SCKI) ---> P51(55) / SCK2、P24(33) / SCK3
	typedef device::PORT<device::PORT4, device::bitpos::B0> LTC_CSN;   // P40(141)
	typedef device::PORT<device::PORTB, device::bitpos::B3> LTC_CNV;
	typedef device::PORT<device::PORT1, device::bitpos::B6> LTC_BUSY;
	typedef device::PORT<device::PORT5, device::bitpos::B3> LTC_PD;    // P53(53)
#ifdef SOFT_SPI
	typedef device::PORT<device::PORT5, device::bitpos::B0> LTC_SDI;
	typedef device::PORT<device::PORT5, device::bitpos::B1> LTC_SCKI;
	typedef device::PORT<device::PORT5, device::bitpos::B2> LTC_SDO0;
	typedef chip::LTC2348_16a<LTC_CSN, LTC_CNV, LTC_BUSY, LTC_SDI, LTC_SCKI, LTC_SDO0> EADC;
#else
	typedef utils::fifo<uint8_t, 256> SPI_BF;
	typedef device::sci_io<device::SCI2, SPI_BF, SPI_BF> LTC_SPI_IO;
	typedef chip::LTC2348_16a<LTC_CSN, LTC_CNV, LTC_BUSY, LTC_SPI_IO> EADC;
#endif
	EADC		eadc_;

	uint16_t ch0_src_[1024+1];
	uint16_t ch1_src_[1024+1];

	volatile uint16_t	cap_num_;
	volatile uint16_t	cap_pos_;

	void capture_request_(uint16_t num)
	{
		if(num > 1024) num = 1024;
		else if(num == 0) return;
		cap_pos_ = 0;
		cap_num_ = num + 1;
	}

	class capture_task {
	public:
		void operator() ()
		{
			if(cap_num_ > 0) {
				LED::P = 1;
				eadc_.convert();
				LED::P = 0;
				--cap_num_;
				ch0_src_[cap_pos_] = eadc_.get_value(0);
				ch1_src_[cap_pos_] = eadc_.get_value(1);	
				++cap_pos_;
			}
		}
	};

	typedef device::tpu_io<device::TPU0, capture_task> TPU0;
	TPU0		tpu0_;

#ifdef UART_DEBUG
	typedef device::PORT<device::PORTF, device::bitpos::B5> TRG;

	typedef utils::fifo<uint8_t, 1024> BUFFER;
	typedef device::sci_io<device::SCI12, BUFFER, BUFFER> D_SCI;
	D_SCI			d_sci_;
#else
	typedef utils::fifo<uint8_t, 1024> BUFFER;
	typedef device::sci_io<device::SCI0,  BUFFER, BUFFER> D_SCI;
	D_SCI			d_sci_;

	typedef utils::fifo<uint8_t, 4096> SENDBUF;
	typedef device::sci_io<device::SCI12, BUFFER, SENDBUF> M_SCI;
	M_SCI			m_sci_;
#endif

	typedef device::dac_out	DAC;
	DAC		dac_;

	volatile uint16_t cap_count_;
	volatile uint16_t irq_count_;
	uint16_t irq_state_;

	// trigger task
	class irq_task {
	public:
		void operator() ()
		{
			if(cap_count_ > 0) {
				capture_request_(cap_count_);
			}
			++irq_count_;
		}
	};

	typedef device::irq_man<device::peripheral::IRQ4, irq_task> IRQ;
	IRQ		irq_;

	TASK	task_ = TASK::STATE;

	uint16_t	length_;
	uint16_t	volt_;
	uint16_t	count_;

	SEND_TASK  	send_task_;

	void send_wave_(char ch, uint16_t src)
	{
		m_sci_.putch(0x01);  // A/D 通知
		m_sci_.putch(ch);    // chanel NO
		m_sci_.putch(src >> 8);
		m_sci_.putch(src & 0xff);
	}


	void send_wave_(char ch, uint16_t len, const uint16_t* src)
	{
		m_sci_.putch(0x01);  // A/D 通知
		m_sci_.putch(ch);    // chanel NO

		for(uint16_t i = 0; i < len; ++i) {
			m_sci_.putch(src[i] >> 8);
			m_sci_.putch(src[i] & 0xff);
		}
	}

	void setup_irq_(bool positive)
	{
		uint8_t int_level = 2;

		IRQ::edge edge = IRQ::edge::POSITIVE;
		if(!positive) edge = IRQ::edge::NEGATIVE;
		bool ret = irq_.start(int_level, edge, device::port_map::option::SECOND);
		if(!ret) {
			utils::format("IRQ start fail...\n");
			return;
		}
	}
}

extern "C" {

	//-----------------------------------------------------------------//
	/*!
		@brief  システム・文字出力
		@param[in]	ch	文字
	*/
	//-----------------------------------------------------------------//
	void sci_putch(char ch)
	{
#if 0
		if(putch_lock_) {
			if((putch_tmp_.size() - putch_tmp_.length()) >= 2) {
				putch_tmp_.put(ch);
			}
			return;
		}
		putch_lock_ = true;
#endif
		d_sci_.putch(ch);
//		putch_lock_ = false;
	}


	//-----------------------------------------------------------------//
	/*!
		@brief  システム・文字列出力
		@param[in]	s	文字列
	*/
	//-----------------------------------------------------------------//
	void sci_puts(const char* s)
	{
		d_sci_.puts(s);
	}


	//-----------------------------------------------------------------//
	/*!
		@brief  システム・文字入力
		@return	文字
	*/
	//-----------------------------------------------------------------//
	char sci_getch(void)
	{
		return d_sci_.getch();
	}


	//-----------------------------------------------------------------//
	/*!
		@brief  システム・文字列長の取得
		@return	文字列長
	*/
	//-----------------------------------------------------------------//
	uint16_t sci_length(void)
	{
		return d_sci_.recv_length();
	}
}


int main(int argc, char** argv);

int main(int argc, char** argv)
{
	device::PORT3::PCR.B5 = 1; // P35(NMI) pull-up

	device::SYSTEM::PRCR = 0xA50B;	// クロック、低消費電力、関係書き込み許可

	device::SYSTEM::MOSCWTCR = 9;	// 1ms wait
	// メインクロック強制発振とドライブ能力設定
	device::SYSTEM::MOFCR = device::SYSTEM::MOFCR.MODRV2.b(0b10)
						  | device::SYSTEM::MOFCR.MOFXIN.b(1);
	device::SYSTEM::MOSCCR.MOSTP = 0;		// メインクロック発振器動作
	while(device::SYSTEM::OSCOVFSR.MOOVF() == 0) asm("nop");

	// Base Clock 12.5MHz
	// PLLDIV: 1/1, STC: 16 倍(200MHz)
	device::SYSTEM::PLLCR = device::SYSTEM::PLLCR.PLIDIV.b(0) |
							device::SYSTEM::PLLCR.STC.b(0b100011);
	device::SYSTEM::PLLCR2.PLLEN = 0;			// PLL 動作
	while(device::SYSTEM::OSCOVFSR.PLOVF() == 0) asm("nop");

	device::SYSTEM::SCKCR = device::SYSTEM::SCKCR.FCK.b(2)		// 1/2 (200/4=50)
						  | device::SYSTEM::SCKCR.ICK.b(1)		// 1/2 (200/2=100)
						  | device::SYSTEM::SCKCR.BCK.b(2)		// 1/2 (200.5/4=50)
						  | device::SYSTEM::SCKCR.PCKA.b(1)		// 1/2 (200.5/2=100)
						  | device::SYSTEM::SCKCR.PCKB.b(2)		// 1/4 (200.5/4=50)
						  | device::SYSTEM::SCKCR.PCKC.b(2)		// 1/4 (200.5/4=50)
						  | device::SYSTEM::SCKCR.PCKD.b(2);	// 1/4 (200.5/4=50)
	device::SYSTEM::SCKCR2 = device::SYSTEM::SCKCR2.UCK.b(0b0100) | 1;  // USB Clock: 1/5 (200/5=40)
	device::SYSTEM::SCKCR3.CKSEL = 0b100;	///< PLL 選択

	LED::DIR = 1;

	{
		// タイマー設定（100Hz）
		uint8_t cmt_irq_level = 3;
		cmt_.start(100, cmt_irq_level);
	}

	{  // TPU0 設定
		uint8_t int_level = 4;
		if(!tpu0_.start(100000, int_level)) {
			utils::format("TPU0 not start ...\n");
		}
	}

	{  // DEBUG SCI 設定
		uint8_t int_level = 1;
		d_sci_.start(115200, int_level);
	}

	{  // MAIN SCI 設定
		uint8_t int_level = 1;
		m_sci_.start(115200, int_level);
		m_sci_.auto_crlf(false);
	}

	{  // D/A CH0, AMP enable
		dac_.start(DAC::output::CH0, true);
	}

	{  // LTC2348ILX-16 初期化
		LTC_PD::DIR = 1;
		LTC_PD::P = 0;  // パワーダウンしない！
		device::port_map::turn(device::peripheral::SCI2, true, device::port_map::option::SECOND);
		// 内臓リファレンスと内臓バッファ
		// VREFIN: 2.024V、VREFBUF: 4.096V、Analog range: -10.24V to +10.24V
///		if(!eadc_.start(750000, EADC::span_type::M10_24P10_24)) {
		if(!eadc_.start(800000, EADC::span_type::M10_24P10_24)) {
			utils::format("LTC2348_16 not found...\n");
		}
	}

	// キャプチャー時間計測
	if(0) {
		while(1) {
			while(cap_num_ > 0) ;
			capture_request_(1024);
		}
	}

	if(0) {
		uint16_t brr = device::SCI2::BRR();
		char tmp[128];
		utils::sformat("BRR: %d\r\n", tmp, sizeof(tmp)) % brr;
		m_sci_.puts(tmp);
	}

	if(0) {  // test wave data
		uint16_t v = 0;
		for(int i = 0; i < 1024; ++i) {
			ch0_src_[i] = v;
			ch1_src_[i] = v + 32768;
			v += 64;
		}
	} else {
		for(int i = 0; i <= 1024; ++i) {
			ch0_src_[i] = 0;
			ch1_src_[i] = 0;
		}
	}

	// キャプチャー
	capture_request_(1024);

	// 初期設定
	dac_.out0(32768);  // トリガー電圧０Ｖ

	irq_count_ = 0;
	irq_state_ = 0;

	uint8_t cnt = 0;
	while(1) {
		cmt_.sync();

		switch(task_) {
		case TASK::STATE:
			if(m_sci_.recv_length() >= 1) {
				CMD cmd = static_cast<CMD>(m_sci_.getch());
				if(cmd == CMD::START) {
					task_ = TASK::LENGTH;
				} else if(cmd == CMD::ASC_CNVTEST) {
					capture_request_(1024);
					send_task_ = SEND_TASK::ASCII;
				} else if(cmd == CMD::ASC_CNVTESTS) {
					capture_request_(1);
					send_task_ = SEND_TASK::ASCII2;
				} else if(cmd == CMD::END) {
					if(send_task_ == SEND_TASK::MULTI) {  // 波形取得を強制終了
						send_task_ = SEND_TASK::READY;
					}
				} else if(cmd == CMD::MODE_SELECT) {


				} else if(cmd == CMD::MODE_SENSE) {


				} else if(cmd == CMD::VER) {


				}
			}
			break;

		case TASK::LENGTH:
			if(m_sci_.recv_length() >= 2) {
				length_  = static_cast<uint8_t>(m_sci_.getch());
				length_ <<= 8;
				length_ |= static_cast<uint8_t>(m_sci_.getch());
				task_ = TASK::VOLT;
			}
			break;

		case TASK::VOLT:
			if(m_sci_.recv_length() >= 2) {
				volt_  = static_cast<uint8_t>(m_sci_.getch());
				volt_ <<= 8;
				volt_ |= static_cast<uint8_t>(m_sci_.getch());
				task_ = TASK::STATE;
				dac_.out0(volt_);  // トリガー電圧設定
				bool trg = true;
				if(volt_ < 32768) trg = false;
				cap_count_ = length_;
				 setup_irq_(trg);
				irq_state_ = irq_count_;
				irq_.enable();
				if(length_ == 0) {
					send_task_ = SEND_TASK::SINGLE;
				} else {
					send_task_ = SEND_TASK::MULTI;
				}
			}
			break;

		default:
			break;
		}

		switch(send_task_) {

		case SEND_TASK::SINGLE:
			if(cap_num_ > 0) break;
			eadc_.convert();
			send_wave_(0x01, eadc_.get_value(0));
			send_wave_(0x02, eadc_.get_value(1));
			send_task_ = SEND_TASK::READY;
			break;

		case SEND_TASK::MULTI:
			if(irq_state_ == irq_count_) break;
			if(cap_num_ > 0) break;
			send_wave_(0x01, length_, ch0_src_ + 1);
			send_wave_(0x02, length_, ch1_src_ + 1);
			send_task_ = SEND_TASK::READY;
			break;

		case SEND_TASK::ASCII:
			if(cap_num_ > 0) break;
			for(uint16_t i = 1; i <= 1024; ++i) {
				char tmp[128];
				utils::sformat("%d,%d,%d\r\n", tmp, sizeof(tmp)) % i % ch0_src_[i] % ch1_src_[i];
				m_sci_.puts(tmp);
			}
			send_task_ = SEND_TASK::READY;
			break;
		case SEND_TASK::ASCII2:
			if(cap_num_ > 0) break;
			{
				char tmp[128];
				utils::sformat("%d,%d\r\n", tmp, sizeof(tmp)) % ch0_src_[1] % ch1_src_[1];
				m_sci_.puts(tmp);
			}
			send_task_ = SEND_TASK::READY;
			break;


		default:
			break;
		}

		++cnt;
		if(cnt >= 50) {
			cnt = 0;
		}
		if(cnt >= 35) {
			LED::P = 1;
		} else {
			LED::P = 0;
		}
	}
}

