#pragma once
//=====================================================================//
/*!	@file
	@brief	RX64M グループ・ペリフェラル @n
			Copyright 2016 Kunihito Hiramatsu
	@author	平松邦仁 (hira@rvf-rc45.net)
*/
//=====================================================================//
#include <cstdint>

namespace device {

	//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
	/*!
		@brief  ペリフェラル種別
	*/
	//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
	enum class peripheral {
		SCI0,  // (P20:TXD0, P21:RXD0)
		SCI1,  // (PF0:TXD1, PF2:RXD1)
		SCI2,  // (P13:TXD2, P12:RXD2)
		SCI3,  // (P23:TXD3, P25:RXD3)
		SCI4,  // (PB1:TXD4, PB0:RXD4)
		SCI5,  // (PA4:TXD5, PA2:RXD5)
		SCI6,  // (P00:TXD6, P01:RXD6)
		SCI7,  // (P90:TXD7, P92:RXD7)

		SCI12,  //

		CMT0,
		CMT1,
		CMT2,
		CMT3,
	};

}