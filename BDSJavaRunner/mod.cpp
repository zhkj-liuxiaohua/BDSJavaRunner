#include "mod.h"
#include "THook.h"
#include "pch.h"
#include <stdio.h>
#include <fstream>
#include <jni.h>
#include <shlwapi.h>
#include <json/json.h>
#include "javas/BDS_MCJAVAAPI.h"
#include "Events.h"
#include "tick/tick.h"
#include <mutex>
#include "GUI/SimpleForm.h"
#include "scoreboard/scoreboard.hpp"
#include "commands/commands.h"

#ifdef _WIN32  
#define PATH_SEPARATOR ';'  
#else  
#define PATH_SEPARATOR ':'  
#endif 

// 当前插件平台版本号
static const wchar_t* VERSION = L"1.16.201.3";
static const wchar_t* ISFORCOMMERCIAL = L"1";
static bool netregok = false;

static void initMods();
HookErrorCode mTHook2(RVA, void*);
void** getOriginalData(void*);
static void shutdownJVM();

static std::mutex mleftlock;

// 调试信息
template<typename T>
static void PR(T arg) {
#ifndef RELEASED
	std::cout << arg << std::endl;
#endif // !RELEASED
}

// 后台指令队列
static VA p_spscqueue = 0;
// 地图信息
static VA p_level = 0;
// 网络信息
static VA p_ServerNetworkHandle = 0;
// 脚本引擎信息
static VA p_jsen = 0;

static ACTEVENT ActEvent;

// 标准输出句柄常量
static const VA STD_COUT_HANDLE = SYM_OBJECT(VA,
	MSSYM_B2UUA3impB2UQA4coutB1AA3stdB2AAA23VB2QDA5basicB1UA7ostreamB1AA2DUB2QDA4charB1UA6traitsB1AA1DB1AA3stdB3AAAA11B1AA1A);

static std::unordered_map<std::string, std::vector<std::pair<JavaVM*, jobject>>*> beforecallbacks, aftercallbacks;
static std::unordered_map<std::string, bool> isListened;	// 是否已监听
static std::unordered_map<std::string, VA*> sListens;		// 监听列表

static std::string JstringToUTF8(JNIEnv* env, jstring s) {
	std::string str;
	if (s) {
		jboolean c = false;
		str = std::string(env->GetStringUTFChars(s, &c));
	}
	return str;
}

static std::wstring toGBKString(std::string s) {
	int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.length(), NULL, 0);
	WCHAR* w_str = new WCHAR[(size_t)len + 1]{ 0 };
	MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.length(), w_str, len);
	auto gbkstr = std::wstring(w_str);
	delete[] w_str;
	return gbkstr;
}
static std::string toUTF8String(std::wstring s) {
	int iSize = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, NULL, 0, NULL, NULL);
	char* uc = new char[(size_t)(iSize) * 3 + 1]{ 0 };
	WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, uc, iSize, NULL, NULL);
	auto utf8str = std::string(uc);
	delete[] uc;
	return utf8str;
}


// 维度ID转换为中文字符
static std::string toDimenStr(int dimensionId) {
	switch (dimensionId) {
	case 0:return u8"主世界";
	case 1:return u8"地狱";
	case 2:return u8"末地";
	default:
		break;
	}
	return u8"未知维度";
}
// 转换字符串为Json对象
static Json::Value toJson(std::string s) {
	Json::Value jv;
	Json::CharReaderBuilder r;
	JSONCPP_STRING errs;
	std::unique_ptr<Json::CharReader> const jsonReader(r.newCharReader());
	bool res = jsonReader->parse(s.c_str(), s.c_str() + s.length(), &jv, &errs);
	if (!res || !errs.empty()) {
		PR(u8"JSON转换失败。。" + errs);
	}
	return jv;
}
// 检查java代码运行期异常
static bool checkException(JNIEnv* env) {
	if (env->ExceptionOccurred())
	{
		env->ExceptionDescribe();
		env->ExceptionClear();
		return true;
	}
	return false;
}

// 执行回调
static bool runJavacode(std::string key, ActMode mode, Json::Value& eventData) {
	auto& funcs = (mode == ActMode::BEFORE) ? beforecallbacks :
		aftercallbacks;
	auto dv = funcs[key];
	bool ret = true;
	if (dv) {
		if (dv->size() > 0) {
			auto sdata = eventData.toStyledString();
			for (auto& func : *dv) {
				try {
					JNIEnv* ev = NULL;
					func.first->AttachCurrentThread((void**)&ev, NULL);
					auto cls = ev->GetObjectClass(func.second);
					auto mid = ev->GetMethodID(cls, "callback", "(Ljava/lang/String;)Z");
					auto jdata = ev->NewStringUTF(sdata.c_str());
					ret = ret && ev->CallBooleanMethod(func.second, mid, jdata);
					checkException(ev);
					ev->DeleteLocalRef(jdata);
					func.first->DetachCurrentThread();
				}
				catch (...) { PR("[JR] Event callback exception."); }
			}
		}
	}
	return ret;
}

static bool addListener(std::string key, ActMode mode, std::pair<JavaVM*, jobject>& cb) {
	auto& funcs = (mode == ActMode::BEFORE) ? beforecallbacks :
		aftercallbacks;
	if (key != "") {
		if (!isListened[key]) {			// 动态挂载hook监听
			VA* syms = sListens[key];
			int hret = 0;
			if (syms) {
				for (VA i = 0, len = syms[0]; i < len; ++i) {								// 首数为sym长度
					hret += (int)mTHook2((RVA)syms[(i * 2) + 1], (void*)syms[(i * 2) + 2]);	// 第一数为sym，第二数为func
				}
			}
			if (hret) {
				PR("[JR] Some hooks wrong at event setting.");
			}
			isListened[key] = true;		// 只挂载一次
		}
		auto dv = funcs[key];
		if (dv == NULL) {
			dv = new std::vector<std::pair<JavaVM*, jobject>>();
			funcs[key] = dv;
		}
		if (std::find(dv->begin(), dv->end(), cb) == dv->end()) {
			// 未找到已有函数，加入回调
			dv->push_back(cb);
			return true;
		}
	}
	return false;
}

static void remove_v2(std::vector<std::pair<JavaVM*, jobject>>* v, std::pair<JavaVM*, jobject>& val) {
	v->erase(std::remove(v->begin(), v->end(), val), v->end());
}

static bool removeListener(std::string key, ActMode mode, std::pair<JNIEnv*, jobject>& cb) {
	auto& funcs = (mode == ActMode::BEFORE) ? beforecallbacks :
		aftercallbacks;
	if (key != "") {
		auto dv = funcs[key];
		if (dv != NULL) {
			bool exi = false;
			std::pair<JavaVM*, jobject>* fineddata = NULL;
			for (auto& d : *dv) {
				if (cb.first->IsSameObject(d.second, cb.second)) {
					exi = true;
					fineddata = &d;
					break;
				}
			}
			if (exi) {
				auto jobj = fineddata->second;
				remove_v2(dv, *fineddata);
				cb.first->DeleteGlobalRef(jobj);
			}
			return exi;
		}
	}
	return false;
}

JNIEXPORT void JNICALL Java_BDS_MCJAVAAPI_log
(JNIEnv* ev, jobject args, jstring s) {
	std::cout << JstringToUTF8(ev, s) << std::endl;
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_addBeforeActListener
(JNIEnv* ev, jobject api, jstring key, jobject cb) {
	if (!key || !cb)
		return false;
	auto gcb = ev->NewGlobalRef(cb);
	JavaVM* jvm;
	ev->GetJavaVM(&jvm);
	std::pair<JavaVM*, jobject> eventcb(jvm, gcb);
	return addListener(JstringToUTF8(ev, key), ActMode::BEFORE, eventcb);
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_addAfterActListener
(JNIEnv* ev, jobject api, jstring key, jobject cb) {
	if (!key || !cb)
		return false;
	auto gcb = ev->NewGlobalRef(cb);
	JavaVM* jvm;
	ev->GetJavaVM(&jvm);
	std::pair<JavaVM*, jobject> eventcb(jvm, gcb);
	return addListener(JstringToUTF8(ev, key), ActMode::AFTER, eventcb);
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_removeBeforeActListener
(JNIEnv* ev, jobject api, jstring key, jobject cb) {
	if (!key || !cb)
		return false;
	std::pair<JNIEnv*, jobject> eventcb(ev, cb);
	return removeListener(JstringToUTF8(ev, key), ActMode::BEFORE, eventcb);
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_removeAfterActListener
(JNIEnv* ev, jobject api, jstring key, jobject cb) {
	if (!key || !cb)
		return false;
	std::pair<JNIEnv*, jobject> eventcb(ev, cb);
	return removeListener(JstringToUTF8(ev, key), ActMode::AFTER, eventcb);
}
JNIEXPORT void JNICALL Java_BDS_MCJAVAAPI_postTick
(JNIEnv* ev, jobject api, jobject cb) {
	if (!cb)
		return;
	JavaVM* jvm;
	ev->GetJavaVM(&jvm);
	auto gcb = ev->NewGlobalRef(cb);
	auto f = [jvm, gcb]() {
		JNIEnv* cev = NULL;
		jvm->AttachCurrentThread((void**)&cev, NULL);
		auto cls = cev->GetObjectClass(gcb);
		auto mid = cev->GetMethodID(cls, "callback", "()V");
		cev->CallVoidMethod(gcb, mid);
		checkException(cev);
		cev->DeleteGlobalRef(gcb);
		jvm->DetachCurrentThread();
	};
	safeTick(f);
}
static VA regHandle = 0;		// 指令注册指针
struct CmdDescriptionFlags {
	std::string description;
	char level;
	char flag1;
	char flag2;
};
static std::unordered_map<std::string, std::unique_ptr<CmdDescriptionFlags>> cmddescripts;
// 函数名：setCommandDescribeEx
// 功能：设置一个全局指令说明
// 参数个数：2个
// 参数类型：字符串，字符串，整型，整型，整型
// 参数详解：cmd - 命令，description - 命令说明，level - 执行要求等级，flag1 - 命令类型1， flag2 - 命令类型2
// 备注：延期注册的情况，可能不会改变客户端界面
static void setCommandDescribeEx(std::string cmd, std::string description, char level, char flag1, char flag2) {
	auto &strcmd = cmd;
	if (strcmd.length()) {
		auto flgs = std::make_unique<CmdDescriptionFlags>();
		flgs->description = description;
		flgs->level = level;
		flgs->flag1 = flag1;
		flgs->flag2 = flag2;
		cmddescripts[strcmd] = std::move(flgs);
		if (regHandle) {
			std::string c = strcmd;
			auto &ct = description;
			SYMCALL(VA, MSSYM_MD5_8574de98358ff66b5a913417f44dd706,		// CommandRegistry::registerCommand
				regHandle, &c, ct.c_str(), level, flag1, flag2);
		}
	}
}
JNIEXPORT void JNICALL Java_BDS_MCJAVAAPI_setCommandDescribe
(JNIEnv* ev, jobject api, jstring cmd, jstring des) {
	auto ccmd = JstringToUTF8(ev, cmd);
	auto cdes = JstringToUTF8(ev, des);
	setCommandDescribeEx(ccmd, cdes, 0, 0x40, 0);
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_runcmd
(JNIEnv* ev, jobject api, jstring cmd) {
	auto strcmd = JstringToUTF8(ev, cmd);
	if (p_spscqueue) {
		auto fr = [strcmd]() {
			SYMCALL(bool, MSSYM_MD5_b5c9e566146b3136e6fb37f0c080d91e,	// SPSCQueue<>::inner_enqueue<>
				p_spscqueue, strcmd);
		};
		safeTick(fr);
		return true;
	}
	return false;
}
JNIEXPORT void JNICALL Java_BDS_MCJAVAAPI_logout
(JNIEnv* ev, jobject api, jstring cmdout) {
	std::string strout = JstringToUTF8(ev, cmdout) + "\n";
	SYMCALL(VA, MSSYM_MD5_b5f2f0a753fc527db19ac8199ae8f740,		// std::_Insert_string
		STD_COUT_HANDLE, strout.c_str(), strout.length());
}

static std::unordered_map<std::string, Player*> onlinePlayers;
static std::unordered_map<Player*, bool> playerSign;

JNIEXPORT jstring JNICALL Java_BDS_MCJAVAAPI_getOnLinePlayers
(JNIEnv* ev, jobject api) {
	Json::Value rt;
	Json::Value jv;
	mleftlock.lock();
	for (auto& op : playerSign) {
		Player* p = op.first;
		if (op.second) {
			jv["playername"] = p->getNameTag();
			jv["uuid"] = p->getUuid()->toString();
			jv["xuid"] = p->getXuid(p_level);
			jv["playerptr"] = (VA)p;
			rt.append(jv);
		}
	}
	mleftlock.unlock();
	auto rstr = rt.isNull() ? "" : rt.toStyledString();
	return ev->NewStringUTF(rstr.c_str());
}
// 自增长临时脚本ID
static VA tmpjsid = 0;
// 获取临时脚本ID值，调用一次自增长一次
static VA getTmpJsId() {
	return tmpjsid++;
}
JNIEXPORT void JNICALL Java_BDS_MCJAVAAPI_JSErunScript
(JNIEnv* ev, jobject api, jstring js, jobject cb) {
	if (p_jsen) {
		std::string uc = JstringToUTF8(ev, js);
		JavaVM* jvm;
		ev->GetJavaVM(&jvm);
		jobject callb = !cb ? 0 : ev->NewGlobalRef(cb);
		auto fn = [jvm, uc, callb]() {
			std::string name = u8"JR_tmpscript_" + std::to_string(getTmpJsId());
			std::string rfcontent = u8"(function(){\n" + uc + u8"\n}())";
			bool ret = SYMCALL(bool, MSSYM_MD5_23dc46771eb6a97b3d65e0e79a6fed42,	// ScriptApi::ScriptFramework::runScript
				p_jsen, name, uc);
			if (callb) {
				JNIEnv* cev = NULL;
				jvm->AttachCurrentThread((void**)&cev, NULL);
				auto cls = cev->GetObjectClass(callb);
				auto mid = cev->GetMethodID(cls, "callback", "(Z)V");
				cev->CallVoidMethod(callb, mid, ret);
				checkException(cev);
				cev->DeleteGlobalRef(callb);
				jvm->DetachCurrentThread();
			}
		};
		safeTick(fn);
	}
	else {
		if (cb) {
			auto cls = ev->GetObjectClass(cb);
			auto mid = ev->GetMethodID(cls, "callback", "(Z)V");
			ev->CallVoidMethod(cb, mid, false);
			checkException(ev);
		}
	}
}
struct JSON_VALUE {
	VA fill[2]{ 0 };
public:
	JSON_VALUE() {
		SYMCALL(VA, MSSYM_B2QQA60ValueB1AA4JsonB2AAA4QEAAB1AE11W4ValueTypeB1AA11B2AAA1Z, this, 1);
	}
	JSON_VALUE(const char* c) {
		SYMCALL(VA, MSSYM_B2QQA60ValueB1AA4JsonB2AAA4QEAAB1AA4PEBDB1AA1Z, this, c);
	}
	~JSON_VALUE() {
		SYMCALL(VA, MSSYM_B2QQA61ValueB1AA4JsonB2AAA4QEAAB1AA2XZ, this);
	}
};
struct ScriptEventData {
	VA vtabl;
	std::string eventName;
};
struct CustomScriptEventData : ScriptEventData {
	JSON_VALUE mdata;
};
JNIEXPORT void JNICALL Java_BDS_MCJAVAAPI_JSEfireCustomEvent
(JNIEnv* ev, jobject api, jstring ename, jstring jdata, jobject cb) {
	if (p_jsen) {
		std::string name = JstringToUTF8(ev, ename);
		std::string data = JstringToUTF8(ev, jdata);
		jobject callb = !cb ? 0 : ev->NewGlobalRef(cb);
		JavaVM* jvm;
		ev->GetJavaVM(&jvm);
		auto fn = [jvm, name, data, callb]() {
			CustomScriptEventData edata;
			edata.vtabl = (VA)SYM_POINT(VA, MSSYM_B3QQUE227CustomScriptEventDataB2AAA26BB1A);
			edata.eventName = name;
			SYMCALL(VA, MSSYM_B2QQA60ValueB1AA4JsonB2AAA4QEAAB1AA4PEBDB1AA1Z, &edata.mdata, data.c_str());
			bool ret = SYMCALL(bool, MSSYM_B1QA9fireEventB3AQDE23ScriptEngineWithContextB1AE20VScriptServerContextB4AAAAA4QEAAB1UE20NAEBUScriptEventDataB3AAAA1Z,
				p_jsen, &edata);
			if (callb) {
				JNIEnv* cev = NULL;
				jvm->AttachCurrentThread((void**)&cev, NULL);
				auto cls = cev->GetObjectClass(callb);
				auto mid = cev->GetMethodID(cls, "callback", "(Z)V");
				cev->CallVoidMethod(callb, mid, ret);
				checkException(cev);
				cev->DeleteGlobalRef(callb);
				jvm->DetachCurrentThread();
			}
		};
		safeTick(fn);
	}
	else {
		if (cb != nullptr) {
			auto cls = ev->GetObjectClass(cb);
			auto mid = ev->GetMethodID(cls, "callback", "(Z)V");
			ev->CallVoidMethod(cb, mid, false);
			checkException(ev);
		}
	}
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_reNameByUuid
(JNIEnv* ev, jobject api, jstring juuid, jstring jnewName) {
	bool ret = false;
	std::string uuid = JstringToUTF8(ev, juuid);
	std::string newName = JstringToUTF8(ev, jnewName);
	Player* taget = onlinePlayers[uuid];
	if (playerSign[taget]) {
		auto fr = [uuid, newName]() {
			Player* p = onlinePlayers[uuid];
			if (playerSign[p]) {
				p->reName(newName);
			}
		};
		safeTick(fr);
		ret = true;
	}
	return ret;
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_addPlayerItem
(JNIEnv* ev, jobject api, jstring juuid, jint id, jshort aux, jbyte count) {
	auto uuid = JstringToUTF8(ev, juuid);
	Player* p = onlinePlayers[uuid];
	bool ret = false;
	if (playerSign[p]) {
		std::string suuid = uuid;
		auto fr = [suuid, id, aux, count]() {
			Player* p = onlinePlayers[suuid];
			if (playerSign[p]) {
				ItemStack x;
				x.getFromId(id, aux, count);
				p->addItem((VA)&x);
				p->updateInventory();
			}
		};
		safeTick(fr);
		ret = true;
	}
	return ret;
}
// 在JSON数据中附上玩家基本信息
static void addPlayerJsonInfo(Json::Value& jv, Player* p) {
	if (p) {
		jv["playername"] = p->getNameTag();
		int did = p->getDimensionId();
		jv["dimensionid"] = did;
		jv["dimension"] = toDimenStr(did);
		jv["isstand"] = p->isStand();
		jv["XYZ"] = toJson(p->getPos()->toJsonString());
		jv["playerptr"] = (VA)p;
	}
}
JNIEXPORT jstring JNICALL Java_BDS_MCJAVAAPI_selectPlayer
(JNIEnv* ev, jobject api, jstring juuid) {
	auto uuid = JstringToUTF8(ev, juuid);
	Player* p = onlinePlayers[uuid];
	std::string rstr;
	if (playerSign[p]) {
		mleftlock.lock();
		Json::Value jv;
		addPlayerJsonInfo(jv, p);
		jv["uuid"] = p->getUuid()->toString();
		jv["xuid"] = p->getXuid(p_level);
#if (COMMERCIAL)
		jv["health"] = p->getAttr("health");
#endif
		mleftlock.unlock();
		rstr = jv.toStyledString();
	}
	return ev->NewStringUTF(rstr.c_str());
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_talkAs
(JNIEnv* ev, jobject api, jstring juuid, jstring jmsg) {
	auto uuid = JstringToUTF8(ev, juuid);
	Player* p = onlinePlayers[uuid];
	if (playerSign[p]) {								// IDA ServerNetworkHandler::handle, https://github.com/NiclasOlofsson/MiNET/blob/master/src/MiNET/MiNET/Net/MCPE%20Protocol%20Documentation.md
		std::string suuid = uuid;
		std::string txt = JstringToUTF8(ev, jmsg);
		auto fr = [suuid, txt]() {
			Player* p = onlinePlayers[suuid];
			if (playerSign[p]) {
				std::string n = p->getNameTag();
				VA nid = p->getNetId();
				VA tpk;
				TextPacket sec;
				SYMCALL(VA, MSSYM_B1QE12createPacketB1AE16MinecraftPacketsB2AAA2SAB1QA2AVB2QDA6sharedB1UA3ptrB1AA7VPacketB3AAAA3stdB2AAE20W4MinecraftPacketIdsB3AAAA1Z,
					&tpk, 9);
				*(char*)(tpk + 40) = 1;
				memcpy((void*)(tpk + 48), &n, sizeof(n));
				memcpy((void*)(tpk + 80), &txt, sizeof(txt));
				SYMCALL(VA, MSSYM_B1QA6handleB1AE20ServerNetworkHandlerB2AAE26UEAAXAEBVNetworkIdentifierB2AAE14AEBVTextPacketB3AAAA1Z,
					p_ServerNetworkHandle, nid, tpk);
			}
		};
		safeTick(fr);
		return true;
	}
	return false;
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_runcmdAs
(JNIEnv* ev, jobject api, jstring juuid, jstring jcmd) {
	auto uuid = JstringToUTF8(ev, juuid);
	Player* p = onlinePlayers[uuid];
	if (playerSign[p]) {
		std::string suuid = uuid;
		std::string scmd = JstringToUTF8(ev, jcmd);
		auto fr = [suuid, scmd]() {
			Player* p = onlinePlayers[suuid];
			if (playerSign[p]) {
				VA nid = p->getNetId();
				VA tpk;
				CommandRequestPacket src;
				SYMCALL(VA, MSSYM_B1QE12createPacketB1AE16MinecraftPacketsB2AAA2SAB1QA2AVB2QDA6sharedB1UA3ptrB1AA7VPacketB3AAAA3stdB2AAE20W4MinecraftPacketIdsB3AAAA1Z,
					&tpk, 76);
				memcpy((void*)(tpk + 40), &scmd, sizeof(scmd));
				SYMCALL(VA, MSSYM_B1QA6handleB1AE20ServerNetworkHandlerB2AAE26UEAAXAEBVNetworkIdentifierB2AAE24AEBVCommandRequestPacketB3AAAA1Z,
					p_ServerNetworkHandle, nid, tpk);
			}
		};
		safeTick(fr);
		return true;
	}
	return false;
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_disconnectClient
(JNIEnv* ev, jobject api, jstring juuid, jstring jtips) {
	auto uuid = JstringToUTF8(ev, juuid);
	Player* p = onlinePlayers[uuid];
	if (playerSign[p]) {
		std::string suuid = uuid;
		std::string stips = "disconnectionScreen.disconnected";
		if (jtips) {
			auto s = JstringToUTF8(ev, jtips);
			if (s.length())
				stips = s;
		}
		auto fr = [suuid, stips]() {
			Player* p = onlinePlayers[suuid];
			if (playerSign[p]) {
				VA nid = p->getNetId();
				SYMCALL(VA, MSSYM_MD5_389e602d185eac21ddcc53a5bb0046ee,		// ServerNetworkHandler::disconnectClient
					p_ServerNetworkHandle, nid, 0, stips, 0);
			}
		};
		safeTick(fr);
		return true;
	}
	return false;
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_sendText
(JNIEnv* ev, jobject api, jstring juuid, jstring jtxt) {
	auto uuid = JstringToUTF8(ev, juuid);
	Player* p = onlinePlayers[uuid];
	if (playerSign[p]) {
		std::string suuid = uuid;
		std::string stxt = "";
		if (jtxt) {
			auto s = JstringToUTF8(ev, jtxt);
			if (s.length())
				stxt = s;
		}
		if (stxt.length() > 0) {
			auto fr = [suuid, stxt]() {
				Player* p = onlinePlayers[suuid];
				if (playerSign[p]) {
					std::string n = p->getNameTag();
					VA nid = p->getNetId();
					VA tpk;
					TextPacket sec;
					SYMCALL(VA, MSSYM_B1QE12createPacketB1AE16MinecraftPacketsB2AAA2SAB1QA2AVB2QDA6sharedB1UA3ptrB1AA7VPacketB3AAAA3stdB2AAE20W4MinecraftPacketIdsB3AAAA1Z,
						&tpk, 9);
					*(char*)(tpk + 40) = 0;
					*(std::string*)(tpk + 48) = n;
					*(std::string*)(tpk + 80) = stxt;
					p->sendPacket(tpk);
				}
			};
			safeTick(fr);
			return true;
		}
	}
	return false;
}

static std::map<unsigned, bool> fids;

// 获取一个未被使用的基于时间秒数的id
static unsigned getFormId() {
	unsigned id = (unsigned)(time(0) + rand());
	do {
		++id;
	} while (id == 0 || fids[id]);
	fids[id] = true;
	return id;
}
// 发送一个SimpleForm的表单数据包
static unsigned sendForm(std::string uuid, std::string str)
{
	unsigned fid = getFormId();
	// 此处自主创建包
	auto fr = [uuid, fid, str]() {
		Player* p = onlinePlayers[uuid];
		if (playerSign[p]) {
			VA tpk;
			ModalFormRequestPacket sec;
			SYMCALL(VA, MSSYM_B1QE12createPacketB1AE16MinecraftPacketsB2AAA2SAB1QA2AVB2QDA6sharedB1UA3ptrB1AA7VPacketB3AAAA3stdB2AAE20W4MinecraftPacketIdsB3AAAA1Z,
				&tpk, 100);
			*(VA*)(tpk + 40) = fid;
			*(std::string*)(tpk + 48) = str;
			p->sendPacket(tpk);
		}
	};
	safeTick(fr);
	return fid;
}
// 函数名：releaseForm
// 功能：放弃一个表单
// 参数个数：1个
// 参数类型：整型
// 参数详解：formid - 表单id
// 返回值：是否释放成功
//（备注：已被接收到的表单会被自动释放）
static bool releaseForm(unsigned fid)
{
	if (fids[fid]) {
		fids.erase(fid);
		return true;
	}
	return false;
}
JNIEXPORT jint JNICALL Java_BDS_MCJAVAAPI_sendSimpleForm
(JNIEnv* ev, jobject api, jstring juuid, jstring jtitle, jstring jcontent, jstring jbuttons) {
	auto uuid = JstringToUTF8(ev, juuid);
	Player* p = onlinePlayers[uuid];
	if (!playerSign[p])
		return 0;
	auto stitle = JstringToUTF8(ev, jtitle);
	auto scontent = JstringToUTF8(ev, jcontent);
	auto sbuttons = JstringToUTF8(ev, jbuttons);
	Json::Value bts;
	Json::Value ja = toJson(sbuttons);
	for (unsigned i = 0; i < ja.size(); i++) {
		Json::Value bt;
		bt["text"] = ja[i];
		bts.append(bt);
	}
	if (bts.isNull())
		bts = toJson("[]");
	std::string str = createSimpleFormString(stitle, scontent, bts);
	return sendForm(uuid, str);
}
JNIEXPORT jint JNICALL Java_BDS_MCJAVAAPI_sendModalForm
(JNIEnv* ev, jobject api, jstring juuid, jstring jtitle, jstring jcontent, jstring jbutton1, jstring jbutton2) {
	auto uuid = JstringToUTF8(ev, juuid);
	Player* p = onlinePlayers[uuid];
	if (!playerSign[p])
		return 0;
	auto utitle = JstringToUTF8(ev, jtitle);
	auto ucontent = JstringToUTF8(ev, jcontent);
	auto ubutton1 = JstringToUTF8(ev, jbutton1);
	auto ubutton2 = JstringToUTF8(ev, jbutton2);
	std::string str = createModalFormString(utitle, ucontent, ubutton1, ubutton2);
	return sendForm(uuid, str);
}
JNIEXPORT jint JNICALL Java_BDS_MCJAVAAPI_sendCustomForm
(JNIEnv* ev, jobject api, jstring juuid, jstring jdata) {
	auto uuid = JstringToUTF8(ev, juuid);
	Player* p = onlinePlayers[uuid];
	if (!playerSign[p])
		return 0;
	auto ujson = JstringToUTF8(ev, jdata);
	return sendForm(uuid, ujson);
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_releaseForm
(JNIEnv* ev, jobject api, jint id) {
	return releaseForm(id);
}
JNIEXPORT jint JNICALL Java_BDS_MCJAVAAPI_getscoreboard
(JNIEnv* ev, jobject api, jstring juuid, jstring jobjname) {
	auto uuid = JstringToUTF8(ev, juuid);
	Player* p = onlinePlayers[uuid];
	if (!playerSign[p])
		return 0;
	auto oname = JstringToUTF8(ev, jobjname);
	return getscoreboard(p, oname);
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_setscoreboard
(JNIEnv* ev, jobject api, jstring juuid, jstring jobjname, jint count) {
	auto uuid = JstringToUTF8(ev, juuid);
	Player* p = onlinePlayers[uuid];
	if (!playerSign[p])
		return false;
	auto oname = JstringToUTF8(ev, jobjname);
	return setscoreboard(p, oname, count);
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_setServerMotd
(JNIEnv* ev, jobject api, jstring jmotd, jboolean isShow) {
	if (p_ServerNetworkHandle) {
		std::string nmotd = JstringToUTF8(ev, jmotd);
		SYMCALL(VA, MSSYM_MD5_21204897709106ba1d290df17fead479,		// ServerNetworkHandler::allowIncomingConnections
			p_ServerNetworkHandle, &nmotd, isShow);
		return true;
	}
	return false;
}

extern Scoreboard* scoreboard;

// 找到一个ID
static bool findScoreboardId(__int64 id, void* outid) {
	struct RDATA { VA fill[3]; };
	std::vector<RDATA> rs;
	SYMCALL(VA, MSSYM_MD5_6f25833219497c9d31c1506cbfc7798a,		// Scoreboard::getScoreboardIdentityRefs
		scoreboard, &rs);
	bool finded = false;
	if (rs.size() > 0)
	{
		for (auto& x : rs) {
			if (x.fill[2] == id) {
				memcpy(outid, &(x.fill[1]), 16);
				finded = true;
				break;
			}
		}
	}
	return finded;
}
JNIEXPORT jint JNICALL Java_BDS_MCJAVAAPI_getscoreById
(JNIEnv* ev, jobject api, jlong id, jstring jobjname) {
	if (!scoreboard)
		return 0;
	auto objname = JstringToUTF8(ev, jobjname);
	auto testobj = scoreboard->getObjective(objname);
	if (!testobj) {
		std::cout << u8"[JR] 未能找到对应计分板，自动创建：" << objname << std::endl;
		testobj = scoreboard->addObjective(objname, objname);
		return 0;
	}
	__int64 a2[2]{0};
	__int64 sid[2]{ 0 };
	sid[0] = id;
	if (findScoreboardId(id, sid)) {
		auto scores = ((Objective*)testobj)->getplayerscoreinfo((ScoreInfo*)a2, (PlayerScoreboardId*)&sid);
		return scores->getcount();
	}
	return 0;
}
JNIEXPORT jint JNICALL Java_BDS_MCJAVAAPI_setscoreById
(JNIEnv* ev, jobject api, jlong id, jstring jobjname, jint count) {
	if (!scoreboard)
		return 0;
	__int64 sid[2]{ 0 };
	sid[0] = id;
	auto objname = JstringToUTF8(ev, jobjname);
	auto testobj = scoreboard->getObjective(objname);
	if (!testobj) {
		std::cout << u8"[JR] 未能找到对应计分板，自动创建: " << objname << std::endl;
		testobj = scoreboard->addObjective(objname, objname);
	}
	if (!testobj)
		return 0;
	if (!findScoreboardId(id, sid)) {
		SYMCALL(VA, MSSYM_MD5_14cadc365e3074bcb41294b99dc5041d,		// Scoreboard::registerScoreboardIdentity
			scoreboard, &sid, std::to_string(id));
	}
	return scoreboard->modifyPlayerScore((ScoreboardId*)sid, (Objective*)testobj, count);
}

// 附加玩家信息
static void addPlayerInfo(Json::Value& pe, Player* p) {
	pe["playerPtr"] = (VA)p;
	pe["playername"] = p->getNameTag();
	auto did = p->getDimensionId();
	pe["dimensionid"] = did;
	pe["dimension"] = toDimenStr(did);
	Json::Value xyz;
	auto pos = p->getPos();
	pe["XYZ"] = p->getPos()->toJson();
	pe["isstand"] = p->isStand();
}

static void addPlayerDieInfo(Json::Value& ue, Player* pPlayer) {
	ue["playerPtr"] = (VA)pPlayer;
	ue["playername"] = pPlayer->getNameTag();
	auto did = pPlayer->getDimensionId();
	ue["dimensionid"] = did;
	ue["dimension"] = toDimenStr(did);
	ue["XYZ"] = pPlayer->getPos()->toJson();
	ue["isstand"] = pPlayer->isStand();
}

static void addMobDieInfo(Json::Value& ue, Mob* p) {
	ue["mobPtr"] = (VA)p;
	ue["mobname"] = p->getNameTag();
	int did = p->getDimensionId();
	ue["dimensionid"] = did;
	ue["XYZ"] = p->getPos()->toJson();
}

// 判断指针是否为玩家列表中指针
static bool checkIsPlayer(void* p) {
	return playerSign[(Player*)p];
}

// 回传伤害源信息
static void getDamageInfo(void* p, void* dsrc, Json::Value& ue) {			// IDA Mob::die
	char v72;
	VA  v2[2]{0};
	v2[0] = (VA)p;
	v2[1] = (VA)dsrc;
	auto v7 = ((Mob*)p)->getLevel();
	auto srActid = (VA*)(*(VA(__fastcall**)(VA, char*))(*(VA*)v2[1] + 64))(
		v2[1], &v72);
	auto SrAct = SYMCALL(Actor*,
		MSSYM_B1QE11fetchEntityB1AA5LevelB2AAE13QEBAPEAVActorB2AAE14UActorUniqueIDB3AAUA1NB1AA1Z,
		v7, *srActid, 0);
	std::string sr_name = "";
	std::string sr_type = "";
	if (SrAct) {
		sr_name = SrAct->getNameTag();
		int srtype = checkIsPlayer(SrAct) ? 319 : SrAct->getEntityTypeId();
		SYMCALL(std::string&, MSSYM_MD5_af48b8a1869a49a3fb9a4c12f48d5a68,		// EntityTypeToLocString
			&sr_type, srtype);
	}
	Json::Value jv;
	if (checkIsPlayer(p)) {
		addPlayerDieInfo(ue, (Player*)p);
		std::string playertype;				// IDA Player::getEntityTypeId
		SYMCALL(std::string&, MSSYM_MD5_af48b8a1869a49a3fb9a4c12f48d5a68, &playertype, 319);
		ue["mobname"] = ue["playername"];
		ue["mobtype"] = playertype;	// "entity.player.name"
	}
	else {
		addMobDieInfo(ue, (Mob*)p);
		ue["mobtype"] = ((Mob*)p)->getEntityTypeName();
	}
	ue["srcname"] = sr_name;
	ue["srctype"] = sr_type;
	ue["dmcase"] = *((int*)dsrc + 2);
}
//////////////////////////////// 组件 API 区域 ////////////////////////////////

#pragma region 类属性相关方法及实现

int Actor::sgetEntityTypeId(Actor* e) {
	return (*(int(__fastcall**)(Actor*))(*(VA*)e + 1288))(e);	// IDA ScriptPositionComponent::applyComponentTo
}

#pragma endregion

//////////////////////////////// 静态 HOOK 区域 ////////////////////////////////

#if 0 // 测试用途

int s = 5;

// 强制报错
static void herror() {
	PR(1 / (s - 5));
}

bool hooked = false;

#endif

// 此处开始接收异步数据			// IDA main
THook2(_JA_MAIN, VA, MSSYM_A4main,
	VA a1, VA a2, VA a3) {
	initMods();
	auto ret = original(a1, a2, a3);
	shutdownJVM();
	return ret;
}

// 获取后台指令队列
THook2(_JA_GETSPSCQUEUE, VA, MSSYM_MD5_3b8fb7204bf8294ee636ba7272eec000,	// SPSCQueue<>::SPSCQueue<>
	VA _this) {
	p_spscqueue = original(_this);
	return p_spscqueue;
}
// 获取游戏初始化时基本信息
THook2(_JA_ONGAMESESSION, VA,
	MSSYM_MD5_9f3b3524a8d04242c33d9c188831f836,		// GameSession::GameSession
	void* a1, void* a2, VA* a3, void* a4, void* a5, void* a6, void* a7) {
	p_ServerNetworkHandle = *a3;
	return original(a1, a2, a3, a4, a5, a6, a7);
}
// 获取地图初始化信息
THook2(_JA_LEVELINIT, VA, MSSYM_MD5_96d831b409d1a1984d6ac116f2bd4a55,		// Level::Level
	VA a1, VA a2, VA a3, VA a4, VA a5, VA a6, VA a7, VA a8, VA a9, VA a10, VA a11, VA a12, VA a13) {
	VA level = original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
	p_level = level;
	return level;
}
// 注册指令描述引用自list命令注册
THook2(_JA_ONLISTCMDREG, VA, MSSYM_B1QA5setupB1AE11ListCommandB2AAE22SAXAEAVCommandRegistryB3AAAA1Z,
	VA handle) {
	regHandle = handle;
	for (auto& v : cmddescripts) {
		std::string c = std::string(v.first);
		std::string ct = v.second->description;
		char level = v.second->level;
		char f1 = v.second->flag1;
		char f2 = v.second->flag2;
		SYMCALL(VA, MSSYM_MD5_8574de98358ff66b5a913417f44dd706,		// CommandRegistry::registerCommand
			handle, &c, ct.c_str(), level, f1, f2);
	}
	return original(handle);
}
// 脚本引擎登记
THook2(_JA_ONSCRIPTENGINEINIT, bool, MSSYM_B1QE10initializeB1AE12ScriptEngineB2AAA4UEAAB1UA3NXZ, VA jse) {
	bool r = original(jse);
	if (r) {
		p_jsen = jse;
	}
	return r;
}

// 以下部分为具体事件监听挂载点位，动态HOOK区域（部分事件为静态HOOK）

// 初始化基本事件
static void initBaseEventData(Json::Value& e, EventType t, ActMode m, bool result) {
	e["type"] = (UINT16)t;
	e["mode"] = (UINT16)m;
	e["RESULT"] = result;
}
// 切换基本事件
static void changeBaseEventData(Json::Value& e, ActMode m, bool result) {
	e["mode"] = (UINT16)m;
	e["RESULT"] = result;
}
// 服务器后台输入指令
static bool _JA_ONSERVERCMD(VA _this, std::string* cmd) {
	Json::Value se;
	initBaseEventData(se, EventType::onServerCmd, ActMode::BEFORE, false);
	se["cmd"] = *cmd;
	auto& e = se;
	bool ret = runJavacode(ActEvent.ONSERVERCMD, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(VA, std::string*)) * getOriginalData(_JA_ONSERVERCMD);
		ret = original(_this, cmd);
		changeBaseEventData(se, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONSERVERCMD, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONSERVERCMD_SYMS[] = { 1, MSSYM_MD5_b5c9e566146b3136e6fb37f0c080d91e,		// SPSCQueue<>::inner_enqueue<>
	(VA)_JA_ONSERVERCMD };


// 服务器后台指令输出
static VA _JA_ONSERVERCMDOUTPUT(VA handle, char* str, VA size) {
	auto original = (VA(*)(VA, char*, VA)) * getOriginalData(_JA_ONSERVERCMDOUTPUT);
	if (handle == STD_COUT_HANDLE) {
		Json::Value soe;
		initBaseEventData(soe, EventType::onServerCmdOutput, ActMode::BEFORE, false);
		soe["output"] = str;
		auto& e = soe;
		bool ret = runJavacode(ActEvent.ONSERVERCMDOUTPUT, ActMode::BEFORE, e);
		if (ret) {
			VA result = original(handle, str, size);
			changeBaseEventData(soe, ActMode::AFTER, ret);
			runJavacode(ActEvent.ONSERVERCMDOUTPUT, ActMode::AFTER, e);
			return result;
		}
		return handle;
	}
	return original(handle, str, size);
}
static VA ONSERVERCMDOUTPUT_SYMS[] = { 1, MSSYM_MD5_b5f2f0a753fc527db19ac8199ae8f740,		// std::_Insert_string
	(VA)_JA_ONSERVERCMDOUTPUT };

// 玩家选择表单
static void _JA_ONFORMSELECT(VA _this, VA id, VA handle, ModalFormResponsePacket** fp) {
	auto original = (void(*)(VA, VA, VA, ModalFormResponsePacket**)) * getOriginalData(_JA_ONFORMSELECT);
	ModalFormResponsePacket* fmp = *fp;
	Player* p = SYMCALL(Player*, MSSYM_B2QUE15getServerPlayerB1AE20ServerNetworkHandlerB2AAE20AEAAPEAVServerPlayerB2AAE21AEBVNetworkIdentifierB2AAA1EB1AA1Z,
		handle, id, *(char*)((VA)fmp + 16));
	if (p != NULL) {
		UINT fid = fmp->getFormId();
		if (releaseForm(fid)) {
			Json::Value fse;
			initBaseEventData(fse, EventType::onFormSelect, ActMode::BEFORE, false);
			addPlayerInfo(fse, p);
			fse["uuid"] = p->getUuid()->toString();
			fse["selected"] = fmp->getSelectStr();			// 特别鸣谢：sysca11
			fse["formid"] = fid;
			auto &e = fse;
			bool ret = runJavacode(ActEvent.ONFORMSELECT, ActMode::BEFORE, e);
			if (ret) {
				original(_this, id, handle, fp);
				changeBaseEventData(fse, ActMode::AFTER, ret);
				runJavacode(ActEvent.ONFORMSELECT, ActMode::AFTER, e);
			}
			return;
		}
	}
	original(_this, id, handle, fp);
}
static VA ONFORMSELECT_SYMS[] = { 1, MSSYM_MD5_8b7f7560f9f8353e6e9b16449ca999d2,	// PacketHandlerDispatcherInstance<>::handle
	(VA)_JA_ONFORMSELECT };

// 玩家操作物品
static bool _JA_ONUSEITEM(void* _this, ItemStack* item, BlockPos* pBlkpos, unsigned __int8 a4, void* v5, Block* pBlk) {
	auto pPlayer = *reinterpret_cast<Player**>(reinterpret_cast<VA>(_this) + 8);
	Json::Value ue;
	initBaseEventData(ue, EventType::onUseItem, ActMode::BEFORE, false);
	addPlayerInfo(ue, pPlayer);
	ue["position"]= pBlkpos->getPosition()->toJson();
	ue["itemname"] = item->getName();
	ue["itemid"] = item->getId();
	ue["itemaux"] = item->getAuxValue();
	if (pBlk) {
		ue["blockname"] = pBlk->getLegacyBlock()->getFullName();
		ue["blockid"] = pBlk->getLegacyBlock()->getBlockItemID();
	}
	auto& e = ue;
	bool ret = runJavacode(ActEvent.ONUSEITEM, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(void*, ItemStack*, BlockPos*, unsigned __int8, void*, Block*)) * getOriginalData(_JA_ONUSEITEM);
		ret = original(_this, item, pBlkpos, a4, v5, pBlk);
		changeBaseEventData(ue, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONUSEITEM, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONUSEITEM_SYMS[] = { 1, MSSYM_B1QA9useItemOnB1AA8GameModeB2AAA4UEAAB1UE14NAEAVItemStackB2AAE12AEBVBlockPosB2AAA9EAEBVVec3B2AAA9PEBVBlockB3AAAA1Z,
	(VA)_JA_ONUSEITEM };



// 玩家放置方块
static bool _JA_ONPLACEDBLOCK(BlockSource* _this, Block* pBlk, BlockPos* pBlkpos, unsigned __int8 a4, struct Actor* pPlayer, bool _bool) {
	auto original = (bool(*)(BlockSource*, Block*, BlockPos*, unsigned __int8, Actor*, bool)) * getOriginalData(_JA_ONPLACEDBLOCK);
	if (pPlayer && checkIsPlayer(pPlayer)) {
		Player* pp = (Player*)pPlayer;
		Json::Value pe;
		initBaseEventData(pe, EventType::onPlacedBlock, ActMode::BEFORE, false);
		addPlayerInfo(pe, pp);
		pe["blockid"] = pBlk->getLegacyBlock()->getBlockItemID();
		pe["blockname"] = pBlk->getLegacyBlock()->getFullName();
		pe["position"] = pBlkpos->getPosition()->toJson();
		auto& e = pe;
		bool ret = runJavacode(ActEvent.ONPLACEDBLOCK, ActMode::BEFORE, e);
		if (ret) {
			ret = original(_this, pBlk, pBlkpos, a4, pPlayer, _bool);
			changeBaseEventData(pe, ActMode::AFTER, ret);
			runJavacode(ActEvent.ONPLACEDBLOCK, ActMode::AFTER, e);
		}
		return ret;
	}
	return original(_this, pBlk, pBlkpos, a4, pPlayer, _bool);
}
static VA ONPLACEDBLOCK_SYMS[] = { 1, MSSYM_B1QA8mayPlaceB1AE11BlockSourceB2AAA4QEAAB1UE10NAEBVBlockB2AAE12AEBVBlockPosB2AAE10EPEAVActorB3AAUA1NB1AA1Z ,
(VA)_JA_ONPLACEDBLOCK };

// 玩家破坏方块
static bool _JA_ONDESTROYBLOCK(void* _this, BlockPos* pBlkpos) {
	auto pPlayer = *reinterpret_cast<Player**>(reinterpret_cast<VA>(_this) + 8);
	auto pBlockSource = pPlayer->getRegion();					// IDA GameMode::_destroyBlockInternal
	auto pBlk = pBlockSource->getBlock(pBlkpos);
	Json::Value de;
	initBaseEventData(de, EventType::onDestroyBlock, ActMode::BEFORE, false);
	addPlayerInfo(de, pPlayer);
	de["blockid"] = pBlk->getLegacyBlock()->getBlockItemID();
	de["blockname"] = pBlk->getLegacyBlock()->getFullName();
	de["position"] = pBlkpos->getPosition()->toJson();
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONDESTROYBLOCK, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(void*, BlockPos*)) * getOriginalData(_JA_ONDESTROYBLOCK);
		ret = original(_this, pBlkpos);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONDESTROYBLOCK, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONDESTROYBLOCK_SYMS[] = { 1, MSSYM_B2QUE20destroyBlockInternalB1AA8GameModeB2AAA4AEAAB1UE13NAEBVBlockPosB2AAA1EB1AA1Z,
	(VA)_JA_ONDESTROYBLOCK };

// 玩家开箱准备
static bool _JA_ONCHESTBLOCKUSE(void* _this, Player* pPlayer, BlockPos* pBlkpos) {
	auto pBlockSource = (BlockSource*)((Level*)pPlayer->getLevel())->getDimension(pPlayer->getDimensionId())->getBlockSource();
	auto pBlk = pBlockSource->getBlock(pBlkpos);
	Json::Value de;
	initBaseEventData(de, EventType::onStartOpenChest, ActMode::BEFORE, false);
	addPlayerInfo(de, pPlayer);
	de["blockid"] = pBlk->getLegacyBlock()->getBlockItemID();
	de["blockname"] = pBlk->getLegacyBlock()->getFullName();
	de["position"] = pBlkpos->getPosition()->toJson();
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONSTARTOPENCHEST, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(void*, Player*, BlockPos*)) * getOriginalData(_JA_ONCHESTBLOCKUSE);
		ret = original(_this, pPlayer, pBlkpos);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONSTARTOPENCHEST, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONSTARTOPENCHEST_SYMS[] = { 1, MSSYM_B1QA3useB1AE10ChestBlockB2AAA4UEBAB1UE11NAEAVPlayerB2AAE12AEBVBlockPosB2AAA1EB1AA1Z,
	(VA)_JA_ONCHESTBLOCKUSE };

// 玩家开桶准备
static bool _JA_ONBARRELBLOCKUSE(void* _this, Player* pPlayer, BlockPos* pBlkpos) {
	auto pBlockSource = (BlockSource*)((Level*)pPlayer->getLevel())->getDimension(pPlayer->getDimensionId())->getBlockSource();
	auto pBlk = pBlockSource->getBlock(pBlkpos);
	Json::Value de;
	initBaseEventData(de, EventType::onStartOpenBarrel, ActMode::BEFORE, false);
	addPlayerInfo(de, pPlayer);
	de["blockid"] = pBlk->getLegacyBlock()->getBlockItemID();
	de["blockname"] = pBlk->getLegacyBlock()->getFullName();
	de["position"] = pBlkpos->getPosition()->toJson();
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONSTARTOPENBARREL, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(void*, Player*, BlockPos*)) * getOriginalData(_JA_ONBARRELBLOCKUSE);
		ret = original(_this, pPlayer, pBlkpos);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONSTARTOPENBARREL, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONSTARTOPENBARREL_SYMS[] = { 1, MSSYM_B1QA3useB1AE11BarrelBlockB2AAA4UEBAB1UE11NAEAVPlayerB2AAE12AEBVBlockPosB2AAA1EB1AA1Z,
	(VA)_JA_ONBARRELBLOCKUSE };

// 玩家关闭箱子
static void _JA_ONSTOPOPENCHEST(void* _this, Player* pPlayer) {
	auto real_this = reinterpret_cast<void*>(reinterpret_cast<VA>(_this) - 248);
	auto pBlkpos = reinterpret_cast<BlockActor*>(real_this)->getPosition();
	auto pBlockSource = (BlockSource*)((Level*)pPlayer->getLevel())->getDimension(pPlayer->getDimensionId())->getBlockSource();
	auto pBlk = pBlockSource->getBlock(pBlkpos);
	Json::Value de;
	initBaseEventData(de, EventType::onStopOpenChest, ActMode::BEFORE, false);
	addPlayerInfo(de, pPlayer);
	de["blockid"] = pBlk->getLegacyBlock()->getBlockItemID();
	de["blockname"] = pBlk->getLegacyBlock()->getFullName();
	de["position"] = pBlkpos->getPosition()->toJson();
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONSTOPOPENCHEST, ActMode::BEFORE, e);
	if (ret) {
		auto original = (void(*)(void*, Player*)) * getOriginalData(_JA_ONSTOPOPENCHEST);
		original(_this, pPlayer);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONSTOPOPENCHEST, ActMode::AFTER, e);
	}
}
static VA ONSTOPOPENCHEST_SYMS[] = { 1, MSSYM_B1QA8stopOpenB1AE15ChestBlockActorB2AAE15UEAAXAEAVPlayerB3AAAA1Z,
	(VA)_JA_ONSTOPOPENCHEST };

// 玩家关闭木桶
static void _JA_STOPOPENBARREL(void* _this, Player* pPlayer) {
	auto real_this = reinterpret_cast<void*>(reinterpret_cast<VA>(_this) - 248);
	auto pBlkpos = reinterpret_cast<BlockActor*>(real_this)->getPosition();
	auto pBlockSource = (BlockSource*)((Level*)pPlayer->getLevel())->getDimension(pPlayer->getDimensionId())->getBlockSource();
	auto pBlk = pBlockSource->getBlock(pBlkpos);
	Json::Value de;
	initBaseEventData(de, EventType::onStopOpenBarrel, ActMode::BEFORE, false);
	addPlayerInfo(de, pPlayer);
	de["blockid"] = pBlk->getLegacyBlock()->getBlockItemID();
	de["blockname"] = pBlk->getLegacyBlock()->getFullName();
	de["position"] = pBlkpos->getPosition()->toJson();
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONSTOPOPENBARREL, ActMode::BEFORE, e);
	if (ret) {
		auto original = (void(*)(void*, Player*)) * getOriginalData(_JA_STOPOPENBARREL);
		original(_this, pPlayer);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONSTOPOPENBARREL, ActMode::AFTER, e);
	}
}
static VA ONSTOPOPENBARREL_SYMS[] = { 1, MSSYM_B1QA8stopOpenB1AE16BarrelBlockActorB2AAE15UEAAXAEAVPlayerB3AAAA1Z,
	(VA)_JA_STOPOPENBARREL };

// 玩家放入取出数量
static void _JA_ONSETSLOT(LevelContainerModel* a1, VA a2) {
	auto original = (void(*)(LevelContainerModel*, VA)) * getOriginalData(_JA_ONSETSLOT);
	VA* v3 = *((VA**)a1 + 26);							// IDA LevelContainerModel::_getContainer
	BlockSource* bs = *(BlockSource**)(v3[106] + 88);
	BlockPos* pBlkpos = (BlockPos*)((char*)a1 + 216);
	Block* pBlk = bs->getBlock(pBlkpos);
	short id = pBlk->getLegacyBlock()->getBlockItemID();
	if (id == 54 || id == 130 || id == 146 || id == -203 || id == 205 || id == 218) {	// 非箱子、桶、潜影盒的情况不作处理
		int slot = (int)a2;
		auto v5 = (*(VA(**)(LevelContainerModel*))(*(VA*)a1 + 160))(a1);
		if (v5) {
			ItemStack* pItemStack = (ItemStack*)(*(VA(**)(VA, VA))(*(VA*)v5 + 40))(v5, a2);
			auto nid = pItemStack->getId();
			auto naux = pItemStack->getAuxValue();
			auto nsize = pItemStack->getStackSize();
			auto nname = std::string(pItemStack->getName());
			auto pPlayer = a1->getPlayer();
			Json::Value de;
			initBaseEventData(de, EventType::onSetSlot, ActMode::BEFORE, false);
			addPlayerInfo(de, pPlayer);
			de["itemid"] = nid;
			de["itemcount"] = nsize;
			de["itemname"] = nname;
			de["itemaux"] = naux;
			de["position"] = pBlkpos->toJson();
			de["blockid"] = id;
			de["blockname"] = pBlk->getLegacyBlock()->getFullName();
			de["slot"] = slot;
			auto& e = de;
			bool ret = runJavacode(ActEvent.ONSETSLOT, ActMode::BEFORE, e);
			if (ret) {
				original(a1, a2);
				changeBaseEventData(de, ActMode::AFTER, ret);
				runJavacode(ActEvent.ONSETSLOT, ActMode::AFTER, e);
			}
		}
		else
			original(a1, a2);
	}
	else
		original(a1, a2);
}
static VA ONSETSLOT_SYMS[] = { 1, MSSYM_B1QE23containerContentChangedB1AE19LevelContainerModelB2AAA6UEAAXHB1AA1Z,
	(VA)_JA_ONSETSLOT };

// 玩家切换维度
static bool _JA_ONCHANGEDIMENSION(void* _this, Player* pPlayer, void* req) {
	Json::Value de;
	initBaseEventData(de, EventType::onChangeDimension, ActMode::BEFORE, false);
	addPlayerInfo(de, pPlayer);
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONCHANGEDIMENSION, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(void*, Player*, void*)) * getOriginalData(_JA_ONCHANGEDIMENSION);
		ret = original(_this, pPlayer, req);
		changeBaseEventData(de, ActMode::AFTER, ret);
		if (ret) {
			// 此处刷新玩家信息
			addPlayerInfo(de, pPlayer);
		}
		runJavacode(ActEvent.ONCHANGEDIMENSION, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONCHANGEDIMENSION_SYMS[] = { 1, MSSYM_B2QUE21playerChangeDimensionB1AA5LevelB2AAA4AEAAB1UE11NPEAVPlayerB2AAE26AEAVChangeDimensionRequestB3AAAA1Z ,
	(VA)_JA_ONCHANGEDIMENSION };

// 生物死亡
static void _JA_ONMOBDIE(Mob* _this, void* dmsg) {
	Json::Value de;
	initBaseEventData(de, EventType::onMobDie, ActMode::BEFORE, false);
	getDamageInfo(_this, dmsg, de);
	de["mobPtr"] = (VA)_this;
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONMOBDIE, ActMode::BEFORE, e);
	if (ret) {
		auto original = (void(*)(Mob*, void*)) * getOriginalData(_JA_ONMOBDIE);
		original(_this, dmsg);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONMOBDIE, ActMode::AFTER, e);
	}
}
static VA ONMOBDIE_SYMS[] = { 1, MSSYM_B1QA3dieB1AA3MobB2AAE26UEAAXAEBVActorDamageSourceB3AAAA1Z, (VA)_JA_ONMOBDIE };

// 玩家重生
static void _JA_PLAYERRESPAWN(Player* pPlayer) {
	Json::Value de;
	initBaseEventData(de, EventType::onRespawn, ActMode::BEFORE, false);
	addPlayerInfo(de, pPlayer);
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONRESPAWN, ActMode::BEFORE, e);
	if (ret) {
		auto original = (void(*)(Player*)) * getOriginalData(_JA_PLAYERRESPAWN);
		original(pPlayer);
		changeBaseEventData(de, ActMode::AFTER, ret);
		addPlayerInfo(de, pPlayer);
		runJavacode(ActEvent.ONRESPAWN, ActMode::AFTER, e);
	}
}
static VA ONRESPAWN_SYMS[] = { 1, MSSYM_B1QA7respawnB1AA6PlayerB2AAA7UEAAXXZ, (VA)_JA_PLAYERRESPAWN };

// 聊天消息
static void _JA_ONCHAT(void* _this, std::string& player_name, std::string& target, std::string& msg, std::string& chat_style) {
	Json::Value de;
	initBaseEventData(de, EventType::onChat, ActMode::BEFORE, false);
	de["playername"] = player_name;
	de["target"] = target;
	de["msg"] = msg;
	de["chatstyle"] = chat_style;
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONCHAT, ActMode::BEFORE, e);
	if (ret) {
		auto original = (void(*)(void*, std::string&, std::string&, std::string&, std::string&)) * getOriginalData(_JA_ONCHAT);
		original(_this, player_name, target, msg, chat_style);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONCHAT, ActMode::AFTER, e);
	}
}
static VA ONCHAT_SYMS[] = { 1, MSSYM_MD5_ad251f2fd8c27eb22c0c01209e8df83c,	// MinecraftEventing::fireEventPlayerMessage
	(VA)_JA_ONCHAT };

// 输入文本
static void _JA_ONINPUTTEXT(VA _this, VA id, TextPacket* tp) {
	Player* p = SYMCALL(Player*, MSSYM_B2QUE15getServerPlayerB1AE20ServerNetworkHandlerB2AAE20AEAAPEAVServerPlayerB2AAE21AEBVNetworkIdentifierB2AAA1EB1AA1Z,
		_this, id, *((char*)tp + 16));
	Json::Value de;
	initBaseEventData(de, EventType::onInputText, ActMode::BEFORE, false);
	if (p != NULL) {
		addPlayerInfo(de, p);
	}
	de["msg"] = tp->toString();
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONINPUTTEXT, ActMode::BEFORE, e);
	if (ret) {
		auto original = (void(*)(VA, VA, TextPacket*)) * getOriginalData(_JA_ONINPUTTEXT);
		original(_this, id, tp);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONINPUTTEXT, ActMode::AFTER, e);
	}
}
static VA ONINPUTTEXT_SYMS[] = { 1, MSSYM_B1QA6handleB1AE20ServerNetworkHandlerB2AAE26UEAAXAEBVNetworkIdentifierB2AAE14AEBVTextPacketB3AAAA1Z,
	(VA)_JA_ONINPUTTEXT };

// MakeWP
static Player* MakeWP(CommandOrigin& ori) {
	if (ori.getOriginType() == OriginType::Player) {
		return (Player*)ori.getEntity();
	}
	return 0;
}
// 玩家执行指令
static VA _JA_ONINPUTCOMMAND(VA _this, VA mret, std::shared_ptr<CommandContext> x, char a4) {
	auto original = (VA(*)(VA, VA, std::shared_ptr<CommandContext>, char)) * getOriginalData(_JA_ONINPUTCOMMAND);
	Player* p = MakeWP(x->getOrigin());
	if (p) {
		Json::Value de;
		initBaseEventData(de, EventType::onInputCommand, ActMode::BEFORE, false);
		addPlayerInfo(de, p);
		de["cmd"] = x->getCmd();
		VA mcmd = 0;
		auto& e = de;
		bool ret = runJavacode(ActEvent.ONINPUTCOMMAND, ActMode::BEFORE, e);
		if (ret) {
			mcmd = original(_this, mret, x, a4);
			changeBaseEventData(de, ActMode::AFTER, ret);
			runJavacode(ActEvent.ONINPUTCOMMAND, ActMode::AFTER, e);
		}
		return mcmd;
	}
	return original(_this, mret, x, a4);
}
static VA ONINPUTCOMMAND_SYMS[] = { 1, MSSYM_B1QE14executeCommandB1AE17MinecraftCommandsB2AAA4QEBAB1QE10AUMCRESULTB2AAA1VB2QDA6sharedB1UA3ptrB1AE15VCommandContextB3AAAA3stdB3AAUA1NB1AA1Z,
	(VA)_JA_ONINPUTCOMMAND };

// 玩家加载名字
THook2(_JA_ONCREATEPLAYER, VA,
	MSSYM_B1QE14onPlayerJoinedB1AE16ServerScoreboardB2AAE15UEAAXAEBVPlayerB3AAAA1Z,
	Scoreboard* a1, Player* pPlayer) {
	VA hret = original(a1, pPlayer);
	Json::Value le;
	initBaseEventData(le, EventType::onLoadName, ActMode::BEFORE, false);
	addPlayerInfo(le, pPlayer);
	auto uuid = pPlayer->getUuid()->toString();
	le["uuid"] = uuid;
	le["xuid"] = pPlayer->getXuid(p_level);
	//autoByteCpy(&le.ability, getAbilities(pPlayer).toStyledString().c_str());
	onlinePlayers[uuid] = pPlayer;
	playerSign[pPlayer] = true;
	auto& e = le;
	bool ret = runJavacode(ActEvent.ONLOADNAME, ActMode::BEFORE, e);
	if (ret) {
		changeBaseEventData(le, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONLOADNAME, ActMode::AFTER, e);
	}
	return hret;
}

// 玩家离开游戏
THook2(_JA_ONPLAYERLEFT, void,
	MSSYM_B2QUE12onPlayerLeftB1AE20ServerNetworkHandlerB2AAE21AEAAXPEAVServerPlayerB3AAUA1NB1AA1Z,
	VA _this, Player* pPlayer, char v3) {
	Json::Value le;
	initBaseEventData(le, EventType::onPlayerLeft, ActMode::BEFORE, false);
	addPlayerInfo(le, pPlayer);
	auto uuid = pPlayer->getUuid()->toString();
	le["uuid"] = uuid;
	le["xuid"] = pPlayer->getXuid(p_level);
	//autoByteCpy(&le.ability, getAbilities(pPlayer).toStyledString().c_str());
	auto& e = le;
	bool ret = runJavacode(ActEvent.ONPLAYERLEFT, ActMode::BEFORE, e);
	playerSign[pPlayer] = false;
	playerSign.erase(pPlayer);
	onlinePlayers[uuid] = NULL;
	onlinePlayers.erase(uuid);
	if (ret) {
		original(_this, pPlayer, v3);
		changeBaseEventData(le, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONPLAYERLEFT, ActMode::AFTER, e);
	}
}

// 此处为防止意外崩溃出现，设置常锁
THook2(_JA_ONLOGOUT, VA,
	MSSYM_B3QQUE13EServerPlayerB2AAA9UEAAPEAXIB1AA1Z,
	Player* a1, VA a2) {
	mleftlock.lock();
	if (playerSign[a1]) {				// 非正常登出游戏用户，执行注销
		playerSign[a1] = false;
		playerSign.erase(a1);
		const std::string* uuid = NULL;
		for (auto& p : onlinePlayers) {
			if (p.second == a1) {
				uuid = &p.first;
				break;
			}
		}
		if (uuid)
			onlinePlayers.erase(*uuid);
	}
	mleftlock.unlock();
	return original(a1, a2);
}

// 玩家移动信息构筑
static VA _JA_ONMOVE(void* _this, Player* pPlayer, char v3, int v4, int v5) {
	auto original = (VA(*)(void*, Player*, char, int, int)) * getOriginalData(_JA_ONMOVE);
	VA reg = (beforecallbacks[ActEvent.ONMOVE] != NULL ? beforecallbacks[ActEvent.ONMOVE]->size() : 0) +
		(aftercallbacks[ActEvent.ONMOVE] != NULL ? aftercallbacks[ActEvent.ONMOVE]->size() : 0);
	if (!reg)
		return original(_this, pPlayer, v3, v4, v5);
	VA reto = 0;
	Json::Value de;
	initBaseEventData(de, EventType::onMove, ActMode::BEFORE, false);
	addPlayerInfo(de, pPlayer);
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONMOVE, ActMode::BEFORE, e);
	if (ret) {
		reto = original(_this, pPlayer, v3, v4, v5);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONMOVE, ActMode::AFTER, e);
	}
	return reto;
}
static VA ONMOVE_SYMS[] = { 1, MSSYM_B2QQE170MovePlayerPacketB2AAA4QEAAB1AE10AEAVPlayerB2AAE14W4PositionModeB1AA11B1AA2HHB1AA1Z,
	(VA)_JA_ONMOVE };

// 玩家攻击监听
static bool _JA_ONATTACK(Player* pPlayer, Actor* pa) {
	Json::Value de;
	initBaseEventData(de, EventType::onAttack, ActMode::BEFORE, false);
	addPlayerInfo(de, pPlayer);
	de["attackedentityPtr"] = (VA)pa;
	de["actorpos"] = pa->getPos()->toJson();
	de["actorname"] = pa->getNameTag();
	de["actortype"] = pa->getEntityTypeName();
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONATTACK, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(Player*, Actor*)) * getOriginalData(_JA_ONATTACK);
		ret = original(pPlayer, pa);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONATTACK, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONATTACK_SYMS[] = { 1, MSSYM_B1QA6attackB1AA6PlayerB2AAA4UEAAB1UE10NAEAVActorB3AAAA1Z,
	(VA)_JA_ONATTACK };

// 全图范围爆炸监听
static void _JA_ONLEVELEXPLODE(VA _this, BlockSource* a2, Actor* a3, Vec3* a4, float a5, bool a6, bool a7, float a8, bool a9) {
	Json::Value de;
	initBaseEventData(de, EventType::onLevelExplode, ActMode::BEFORE, false);
	de["position"] = a4->toJson();
	int did = a2 ? a2->getDimensionId() : -1;
	de["dimensionid"] = did;
	de["dimension"] = toDimenStr(did);
	if (a3) {
		de["entity"] = a3->getEntityTypeName();
		de["entityid"] = a3->getEntityTypeId();
		int i = a3->getDimensionId();
		de["dimensionid"] = i;
		de["dimension"] = toDimenStr(i);
	}
	de["explodepower"] = a5;
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONLEVELEXPLODE, ActMode::BEFORE, e);
	if (ret) {
		auto original = (void(*)(VA, BlockSource*, Actor*, Vec3*, float, bool, bool, float, bool)) * getOriginalData(_JA_ONLEVELEXPLODE);
		original(_this, a2, a3, a4, a5, a6, a7, a8, a9);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONLEVELEXPLODE, ActMode::AFTER, e);
	}
}
// 重生锚爆炸监听
static bool _JA_SETRESPWNEXPLOREDE(Player* pPlayer, BlockPos* a2, BlockSource* a3, Level* a4) { // IDA RespawnAnchorBlock::trySetSpawn
	auto original = (bool(*)(Player*, BlockPos*, BlockSource*, Level*)) * getOriginalData(_JA_SETRESPWNEXPLOREDE);
	auto v8 = a3->getBlock(a2);
	if (SYMCALL(int, MSSYM_B3QQDA8getStateB1AA1HB1AA5BlockB2AAE18QEBAHAEBVItemStateB3AAAA1Z,
		v8, SYM_POINT(VA, MSSYM_B1QE19RespawnAnchorChargeB1AE13VanillaStatesB2AAA23VB2QDE16ItemStateVariantB1AA1HB2AAA1B)) <= 0)
	{
		return original(pPlayer, a2, a3, a4);
	}
	struct VA_tmp { VA v; };
	if (a3->getDimensionId() != 1) {
		if (!*(char*)(*((VA*)pPlayer + 107) + 7752)) {
			float pw = SYM_OBJECT(float, MSSYM_B2UUA4realB1AA840a00000);
			if (!*(char*)&(((VA_tmp*)a4)[969].v)) {
				if (pw != 0.0) {
					std::string blkname = v8->getLegacyBlock()->getFullName();
					Json::Value de;
					initBaseEventData(de, EventType::onLevelExplode, ActMode::BEFORE, false);
					de["blockid"] = v8->getLegacyBlock()->getBlockItemID();
					de["blockname"] = v8->getLegacyBlock()->getFullName();
					int did = a3->getDimensionId();
					de["dimensionid"] = did;
					de["dimension"] = toDimenStr(did);
					de["position"] = a2->getPosition()->toJson();
					de["explodepower"] = pw;
					auto& e = de;
					bool ret = runJavacode(ActEvent.ONLEVELEXPLODE, ActMode::BEFORE, e);
					if (ret) {
						ret = original(pPlayer, a2, a3, a4);
						changeBaseEventData(de, ActMode::AFTER, ret);
						runJavacode(ActEvent.ONLEVELEXPLODE, ActMode::AFTER, e);
					}
					return ret;
				}
			}
		}
	}
	return original(pPlayer, a2, a3, a4);
}
static VA ONLEVELEXPLODE_SYMS[] = { 2, MSSYM_B1QA7explodeB1AA5LevelB2AAE20QEAAXAEAVBlockSourceB2AAA9PEAVActorB2AAA8AEBVVec3B2AAA1MB1UA4N3M3B1AA1Z,
	(VA)_JA_ONLEVELEXPLODE,
	MSSYM_B1QE11trySetSpawnB1AE18RespawnAnchorBlockB2AAA2CAB1UE11NAEAVPlayerB2AAE12AEBVBlockPosB2AAE15AEAVBlockSourceB2AAA9AEAVLevelB3AAAA1Z,
	(VA)_JA_SETRESPWNEXPLOREDE };

// 玩家切换护甲
static VA _JA_ONSETARMOR(Player* p, int slot, ItemStack* i) {
	auto original = (VA(*)(Player*, int, ItemStack*)) * getOriginalData(_JA_ONSETARMOR);
	if (checkIsPlayer(p)) {
		ItemStack* pItemStack = i;
		auto nid = pItemStack->getId();
		auto naux = pItemStack->getAuxValue();
		auto nsize = pItemStack->getStackSize();
		auto nname = std::string(pItemStack->getName());
		auto pPlayer = p;
		Json::Value de;
		initBaseEventData(de, EventType::onEquippedArmor, ActMode::BEFORE, false);
		addPlayerInfo(de, pPlayer);
		de["itemid"] = nid;
		de["itemcount"] = nsize;
		de["itemname"] = nname;
		de["itemaux"] = naux;
		de["slot"] = slot;
		de["slottype"] = 0;
		auto& e = de;
		bool ret = runJavacode(ActEvent.ONEQUIPPEDARMOR, ActMode::BEFORE, e);
		if (ret) {
			VA ret = original(p, slot, i);
			changeBaseEventData(de, ActMode::AFTER, ret);
			runJavacode(ActEvent.ONEQUIPPEDARMOR, ActMode::AFTER, e);
			return ret;
		}
		return 0;
	}
	return original(p, slot, i);
}
// 玩家切换主副手
static VA _JA_ONSETCARRIEDITEM(VA v1, Player* p, ItemStack* v3, ItemStack* i, int slot) {
	auto original = (VA(*)(VA, Player*, ItemStack*, ItemStack*, int)) * getOriginalData(_JA_ONSETCARRIEDITEM);
	if (checkIsPlayer(p)) {
		ItemStack* pItemStack = i;
		auto nid = pItemStack->getId();
		auto naux = pItemStack->getAuxValue();
		auto nsize = pItemStack->getStackSize();
		auto nname = std::string(pItemStack->getName());
		auto pPlayer = p;
		Json::Value de;
		initBaseEventData(de, EventType::onEquippedArmor, ActMode::BEFORE, false);
		addPlayerInfo(de, pPlayer);
		de["itemid"] = nid;
		de["itemcount"] = nsize;
		de["itemname"] = nname;
		de["itemaux"] = naux;
		de["slot"] = slot;
		de["slottype"] = 1;
		auto& e = de;
		bool ret = runJavacode(ActEvent.ONEQUIPPEDARMOR, ActMode::BEFORE, e);
		if (ret) {
			VA ret = original(v1, p, v3, i, slot);
			changeBaseEventData(de, ActMode::AFTER, ret);
			runJavacode(ActEvent.ONEQUIPPEDARMOR, ActMode::AFTER, e);
			return ret;
		}
		return 0;
	}
	return original(v1, p, v3, i, slot);
}
static VA ONSETARMOR_SYMS[] = { 2, MSSYM_B1QA8setArmorB1AE12ServerPlayerB2AAE16UEAAXW4ArmorSlotB2AAE13AEBVItemStackB3AAAA1Z,
	(VA)_JA_ONSETARMOR,
	MSSYM_B1QE27sendActorCarriedItemChangedB1AE21ActorEventCoordinatorB2AAE14QEAAXAEAVActorB2AAE16AEBVItemInstanceB2AAE111W4HandSlotB3AAAA1Z,
	(VA)_JA_ONSETCARRIEDITEM };

//玩家升级
static void _JA_ONLEVELUP(Player* pl, int a1) {
	Json::Value le;
	initBaseEventData(le, EventType::onLevelUp, ActMode::BEFORE, false);
	addPlayerInfo(le, pl);
	le["lv"] = a1;
	auto& e = le;
	bool ret = runJavacode(ActEvent.ONLEVELUP, ActMode::BEFORE, e);
	if (ret) {
		auto original = (void(*)(Player*, int)) * getOriginalData(_JA_ONLEVELUP);
		original(pl, a1);
		changeBaseEventData(le, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONLEVELUP, ActMode::AFTER, e);
	}
}
static VA ONLEVELUP_SYMS[] = { 1, MSSYM_B1QA9addLevelsB1AA6PlayerB2AAA6UEAAXHB1AA1Z,
	(VA)_JA_ONLEVELUP };

// 活塞推方块事件
static bool _JA_ONPISTONPUSH(BlockActor* _this, BlockSource* a2, BlockPos* a3, UINT8 a4, UINT8 a5) {
	auto pBlkpos = _this->getPosition();
	auto pBlockSource = a2;
	auto pBlk = _this->getBlock();
	auto ptBlk = pBlockSource->getBlock(a3);
	Json::Value de;
	initBaseEventData(de, EventType::onPistonPush, ActMode::BEFORE, false);
	int did = a2->getDimensionId();
	de["dimensionid"] = did;
	de["dimension"] = toDimenStr(did);
	de["blockid"] = pBlk->getLegacyBlock()->getBlockItemID();
	de["blockname"] = pBlk->getLegacyBlock()->getFullName();
	de["position"] = pBlkpos->getPosition()->toJson();
	de["targetblockid"] = ptBlk->getLegacyBlock()->getBlockItemID();
	de["targetblockname"] = ptBlk->getLegacyBlock()->getFullName();
	de["targetposition"] = a3->toJson();
	de["direction"] = a5;
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONPISTONPUSH, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(void*, BlockSource*, BlockPos*, UINT8, UINT8)) * getOriginalData(_JA_ONPISTONPUSH);
		ret = original(_this, a2, a3, a4, a5);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONPISTONPUSH, ActMode::AFTER, e);
		return ret;
	}
	return ret;
}
static VA ONPISTONPUSH_SYMS[] = { 1, MSSYM_B2QUE19attachedBlockWalkerB1AE16PistonBlockActorB2AAA4AEAAB1UE16NAEAVBlockSourceB2AAE12AEBVBlockPosB2AAA2EEB1AA1Z,
	(VA)_JA_ONPISTONPUSH };

// 箱子合并事件
static BlockSource* chestBlockSource = NULL;
// 预判是否可合并
static bool _JA_ONCHESTCANPAIR(VA a1, VA a2, BlockSource* a3) {
	auto org = (bool(*)(VA, VA, BlockSource*)) * getOriginalData(_JA_ONCHESTCANPAIR);
	bool ret = org(a1, a2, a3);
	if (ret) {
		chestBlockSource = a3;
	}
	return ret;
}
static bool _JA_ONCHESTPAIR(BlockActor* a1, BlockActor* a2, bool a3) {
	auto real_this = a1;
	auto pBlkpos = real_this->getPosition();
	auto did = chestBlockSource->getDimensionId();
	auto pBlk = chestBlockSource->getBlock(pBlkpos);
	auto real_that = a2;
	auto ptBlkpos = real_that->getPosition();
	auto ptBlk = chestBlockSource->getBlock(ptBlkpos);;
	Json::Value de;
	initBaseEventData(de, EventType::onChestPair, ActMode::BEFORE, false);
	de["dimensionid"] = did;
	de["dimension"] = toDimenStr(did);
	de["blockid"] = pBlk->getLegacyBlock()->getBlockItemID();
	de["blockname"] = pBlk->getLegacyBlock()->getFullName();
	de["position"] = pBlkpos->getPosition()->toJson();
	de["targetblockid"] = ptBlk->getLegacyBlock()->getBlockItemID();
	de["targetblockname"] = ptBlk->getLegacyBlock()->getFullName();
	de["targetposition"] = ptBlkpos->getPosition()->toJson();
	auto& e = de;
	bool ret = runJavacode(ActEvent.ONCHESTPAIR, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(BlockActor*, BlockActor*, bool)) * getOriginalData(_JA_ONCHESTPAIR);
		ret = original(a1, a2, a3);
		changeBaseEventData(de, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONCHESTPAIR, ActMode::AFTER, e);
		return ret;
	}
	return ret;
}
static VA ONCHESTPAIR_SYMS[] = { 2, MSSYM_B1QE11canPairWithB1AE15ChestBlockActorB2AAA4QEAAB1UE15NPEAVBlockActorB2AAE15AEAVBlockSourceB3AAAA1Z,
	(VA)_JA_ONCHESTCANPAIR,
	MSSYM_B1QA8pairWithB1AE15ChestBlockActorB2AAE10QEAAXPEAV1B2AUA1NB1AA1Z,
	(VA)_JA_ONCHESTPAIR };

// 生物生成检查监听
static bool _JA_ONMOBSPAWNCHECK(Mob* a1, VA a2) {
	auto original = (bool(*)(Mob*, VA)) * getOriginalData(_JA_ONMOBSPAWNCHECK);
	Json::Value me;
	initBaseEventData(me, EventType::onMobSpawnCheck, ActMode::BEFORE, false);
	int did = a1->getDimensionId();
	me["dimensionid"] = did;
	me["mobPtr"] = a1;
	me["dimension"] = toDimenStr(did);
	me["mobname"] = a1->getNameTag();
	me["mobtype"] = a1->getEntityTypeName();
	me["XYZ"] = a1->getPos()->toJson();
	auto& e = me;
	bool ret = runJavacode(ActEvent.ONMOBSPAWNCHECK, ActMode::BEFORE, e);
	if (ret) {
		ret = original(a1, a2);
		changeBaseEventData(me, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONMOBSPAWNCHECK, ActMode::AFTER, e);
		return ret;
	}
	return ret;
}
static VA ONMOBSPAWNCHECK_SYMS[] = { 1, MSSYM_B1QE15checkSpawnRulesB1AA3MobB2AAA4UEAAB1UA1NB1UA1NB1AA1Z,
	(VA)_JA_ONMOBSPAWNCHECK };

// 玩家丢出物品
static bool _JA_ONDROPITEM(Player* pPlayer, ItemStack* itemStack, bool a3) {
	Json::Value pe;
	initBaseEventData(pe, EventType::onDropItem, ActMode::BEFORE, false);
	addPlayerInfo(pe, pPlayer);
	pe["itemid"] = itemStack->getId();
	pe["itemname"] = itemStack->getName();
	pe["itemaux"] = itemStack->getAuxValue();
	auto& e = pe;
	bool ret = runJavacode(ActEvent.ONDROPITEM, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(Player*, ItemStack*, bool)) * getOriginalData(_JA_ONDROPITEM);
		ret = original(pPlayer, itemStack, a3);
		changeBaseEventData(pe, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONDROPITEM, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONDROPITEM_SYMS[] = { 1,MSSYM_B1QA4dropB1AA6PlayerB2AAA4UEAAB1UE14NAEBVItemStackB3AAUA1NB1AA1Z ,
(VA)_JA_ONDROPITEM };

// 玩家捡起物品
static bool _JA_ONPICKUPITEM(Player* pPlayer, ItemActor* itemactor, int a3, unsigned int a4) {
	ItemStack* itemStack = itemactor->getItemStack();
	Json::Value pe;
	initBaseEventData(pe, EventType::onPickUpItem, ActMode::BEFORE, false);
	addPlayerInfo(pe, pPlayer);
	pe["itemid"] = itemStack->getId();
	pe["itemname"] = itemStack->getName();
	pe["itemaux"] = itemStack->getAuxValue();
	auto& e = pe;
	bool ret = runJavacode(ActEvent.ONPICKUPITEM, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(Player*, ItemActor*, int, unsigned int)) * getOriginalData(_JA_ONPICKUPITEM);
		original(pPlayer, itemactor, a3, a4);
		changeBaseEventData(pe, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONPICKUPITEM, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONPICKUPITEM_SYMS[] = { 1, MSSYM_B1QA4takeB1AA6PlayerB2AAA4QEAAB1UE10NAEAVActorB2AAA2HHB1AA1Z,
	(VA)_JA_ONPICKUPITEM };

// 计分板分数改变
static void _JA_ONSCORECHANGED(Scoreboard* class_this, ScoreboardId* a2, Objective* a3) {
	Json::Value pe;
	initBaseEventData(pe, EventType::onScoreChanged, ActMode::BEFORE, false);
	pe["objectivename"] = a3->getscorename();
	pe["displayname"] = a3->getscoredisplayname();
	pe["scoreboardid"] = a2->getId();
	VA sc[2]{ 0 };
	pe["score"] = a3->getscoreinfo((ScoreInfo*)sc, a2)->getcount();
	auto& e = pe;
	bool ret = runJavacode(ActEvent.ONSCORECHANGED, ActMode::BEFORE, e);
	if (ret) {
		auto original = (void(*)(Scoreboard*, ScoreboardId*, Objective*)) * getOriginalData(_JA_ONSCORECHANGED);
		original(class_this, a2, a3);
		changeBaseEventData(pe, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONSCORECHANGED, ActMode::AFTER, e);
	}
}
static VA ONSCORECHANGED_SYMS[] = { 1, MSSYM_B1QE14onScoreChangedB1AE16ServerScoreboardB2AAE21UEAAXAEBUScoreboardIdB2AAE13AEBVObjectiveB3AAAA1Z,
	(VA)_JA_ONSCORECHANGED };

// 官方脚本引擎初始化
static bool _JA_ONSCRIPTENGINEINIT(VA jse) {
	Json::Value pe;
	initBaseEventData(pe, EventType::onScriptEngineInit, ActMode::BEFORE, false);
	pe["jsePtr"] = jse;
	auto& e = pe;
	bool ret = runJavacode(ActEvent.ONSCRIPTENGINEINIT, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(VA)) * getOriginalData(_JA_ONSCRIPTENGINEINIT);
		ret = original(jse);
		changeBaseEventData(pe, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONSCRIPTENGINEINIT, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONSCRIPTENGINEINIT_SYMS[] = { 1, MSSYM_B1QE10initializeB1AE12ScriptEngineB2AAA4UEAAB1UA3NXZ,
	(VA)_JA_ONSCRIPTENGINEINIT };

// 官方脚本引擎输出日志信息
static bool _JA_ONSCRIPTENGINELOG(VA jse, std::string* log) {
	Json::Value pe;
	initBaseEventData(pe, EventType::onScriptEngineLog, ActMode::BEFORE, false);
	pe["jsePtr"] = jse;
	pe["log"] = *log;
	auto& e = pe;
	bool ret = runJavacode(ActEvent.ONSCRIPTENGINELOG, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(VA)) * getOriginalData(_JA_ONSCRIPTENGINELOG);
		ret = original(jse);
		changeBaseEventData(pe, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONSCRIPTENGINELOG, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONSCRIPTENGINELOG_SYMS[] = { 1, MSSYM_MD5_a46384deb7cfca46ec15102954617155,		// ScriptEngine::onLogReceived
	(VA)_JA_ONSCRIPTENGINELOG };

// 官方脚本引擎执行指令
static bool _JA_ONSCRIPTENGINECMD(VA a1, VA jscmd) {
	std::string* cmd = (std::string*)(jscmd + 8);
	Json::Value pe;
	initBaseEventData(pe, EventType::onScriptEngineCmd, ActMode::BEFORE, false);
	pe["jsePtr"] = p_jsen;
	pe["cmd"] = *cmd;
	auto& e = pe;
	bool ret = runJavacode(ActEvent.ONSCRIPTENGINECMD, ActMode::BEFORE, e);
	if (ret) {
		auto original = (bool(*)(VA, VA)) * getOriginalData(_JA_ONSCRIPTENGINECMD);
		ret = original(a1, jscmd);
		changeBaseEventData(pe, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONSCRIPTENGINECMD, ActMode::AFTER, e);
	}
	return ret;
}
static VA ONSCRIPTENGINECMD_SYMS[] = { 1, MSSYM_B1QE14executeCommandB1AE27MinecraftServerScriptEngineB2AAA4UEAAB1UE18NAEBUScriptCommandB3AAAA1Z ,
	(VA)_JA_ONSCRIPTENGINECMD };

// 系统计分板初始化
static VA _JA_ONSCOREBOARDINIT(VA a1, VA a2, VA a3) {
	Json::Value pe;
	initBaseEventData(pe, EventType::onScoreboardInit, ActMode::BEFORE, false);
	pe["scPtr"] = a1;
	auto& e = pe;
	bool ret = runJavacode(ActEvent.ONSCOREBOARDINIT, ActMode::BEFORE, e);
	if (ret) {
		auto original = (VA(*)(VA, VA, VA)) * getOriginalData(_JA_ONSCOREBOARDINIT);
		VA r = original(a1, a2, a3);
		changeBaseEventData(pe, ActMode::AFTER, ret);
		runJavacode(ActEvent.ONSCOREBOARDINIT, ActMode::AFTER, e);
		return r;
	}
	return 0;
}
static VA ONSCOREBOARDINIT_SYMS[] = { 1, MSSYM_B2QQE170ServerScoreboardB2AAA4QEAAB1AE24VCommandSoftEnumRegistryB2AAE16PEAVLevelStorageB3AAAA1Z,
	(VA)_JA_ONSCOREBOARDINIT };

// 初始化各类hook的事件绑定，基于构造函数
static struct EventSymsInit {
public:
	EventSymsInit() {
		sListens[ActEvent.ONSERVERCMD] = ONSERVERCMD_SYMS;
		sListens[ActEvent.ONSERVERCMDOUTPUT] = ONSERVERCMDOUTPUT_SYMS;
		sListens[ActEvent.ONFORMSELECT] = ONFORMSELECT_SYMS;
		sListens[ActEvent.ONUSEITEM] = ONUSEITEM_SYMS;
		sListens[ActEvent.ONPLACEDBLOCK] = ONPLACEDBLOCK_SYMS;
		sListens[ActEvent.ONDESTROYBLOCK] = ONDESTROYBLOCK_SYMS;
		sListens[ActEvent.ONSTARTOPENCHEST] = ONSTARTOPENCHEST_SYMS;
		sListens[ActEvent.ONSTARTOPENBARREL] = ONSTARTOPENBARREL_SYMS;
		sListens[ActEvent.ONSTOPOPENCHEST] = ONSTOPOPENCHEST_SYMS;
		sListens[ActEvent.ONSTOPOPENBARREL] = ONSTOPOPENBARREL_SYMS;
		sListens[ActEvent.ONSETSLOT] = ONSETSLOT_SYMS;
		sListens[ActEvent.ONCHANGEDIMENSION] = ONCHANGEDIMENSION_SYMS;
		sListens[ActEvent.ONMOBDIE] = ONMOBDIE_SYMS;
		sListens[ActEvent.ONRESPAWN] = ONRESPAWN_SYMS;
		sListens[ActEvent.ONCHAT] = ONCHAT_SYMS;
		sListens[ActEvent.ONINPUTTEXT] = ONINPUTTEXT_SYMS;
		sListens[ActEvent.ONINPUTCOMMAND] = ONINPUTCOMMAND_SYMS;
		isListened[ActEvent.ONLOADNAME] = true;
		isListened[ActEvent.ONPLAYERLEFT] = true;
		sListens[ActEvent.ONMOVE] = ONMOVE_SYMS;
		sListens[ActEvent.ONATTACK] = ONATTACK_SYMS;
		sListens[ActEvent.ONLEVELEXPLODE] = ONLEVELEXPLODE_SYMS;
		sListens[ActEvent.ONEQUIPPEDARMOR] = ONSETARMOR_SYMS;
		sListens[ActEvent.ONLEVELUP] = ONLEVELUP_SYMS;
		sListens[ActEvent.ONPISTONPUSH] = ONPISTONPUSH_SYMS;
		sListens[ActEvent.ONCHESTPAIR] = ONCHESTPAIR_SYMS;
		sListens[ActEvent.ONMOBSPAWNCHECK] = ONMOBSPAWNCHECK_SYMS;
		sListens[ActEvent.ONDROPITEM] = ONDROPITEM_SYMS;
		sListens[ActEvent.ONPICKUPITEM] = ONPICKUPITEM_SYMS;
		sListens[ActEvent.ONSCORECHANGED] = ONSCORECHANGED_SYMS;
		sListens[ActEvent.ONSCRIPTENGINEINIT] = ONSCRIPTENGINEINIT_SYMS;
		sListens[ActEvent.ONSCRIPTENGINELOG] = ONSCRIPTENGINELOG_SYMS;
		sListens[ActEvent.ONSCRIPTENGINECMD] = ONSCRIPTENGINECMD_SYMS;
		sListens[ActEvent.ONSCOREBOARDINIT] = ONSCOREBOARDINIT_SYMS;
		/*

		
		
		
		
		*/
#if (COMMERCIAL)
		isListened[ActEvent.ONMOBHURT] = true;
		isListened[ActEvent.ONBLOCKCMD] = true;
		isListened[ActEvent.ONNPCCMD] = true;
		isListened[ActEvent.ONCOMMANDBLOCKUPDATE] = true;
#endif
	}
} _EventSymsInit;

static char localpath[MAX_PATH] = { 0 };
// 获取BDS完整程序路径
static std::string getLocalPath() {
	if (!localpath[0]) {
		GetModuleFileNameA(NULL, localpath, _countof(localpath));
		for (size_t l = strlen(localpath); l != 0; l--) {
			if (localpath[l] == '\\') {
				localpath[l] = localpath[l + 1] = localpath[l + 2] = 0;
				break;
			}
		}
	}
	return std::string(localpath);
}
static HMODULE GetSelfModuleHandle()
{
	MEMORY_BASIC_INFORMATION mbi;
	return ((::VirtualQuery(GetSelfModuleHandle, &mbi, sizeof(mbi)) != 0) ? (HMODULE)mbi.AllocationBase : NULL);
}
// 获取自身DLL路径
//static std::wstring GetDllPathandVersion() {
//	std::ifstream file;
//	wchar_t curDir[256]{ 0 };
//	GetModuleFileName(GetSelfModuleHandle(), curDir, 256);
//	std::wstring dllandVer = std::wstring(curDir);
//	dllandVer = dllandVer + std::wstring(L",") + std::wstring(VERSION);
//	if (netregok)
//		dllandVer = dllandVer + std::wstring(L",") + std::wstring(ISFORCOMMERCIAL);
//	return dllandVer;
//}


static bool inited = false;

// 循环替换
static std::string replace(std::string& base, std::string src, std::string dst)
{
	size_t pos = 0, srclen = src.size(), dstlen = dst.size();
	while ((pos = base.find(src, pos)) != std::string::npos)
	{
		base.replace(pos, srclen, dst);
		pos += dstlen;
	}
	return base;
}
static std::wstring wreplace(std::wstring& base, std::wstring src, std::wstring dst)
{
	size_t pos = 0, srclen = src.size(), dstlen = dst.size();
	while ((pos = base.find(src, pos)) != std::wstring::npos)
	{
		base.replace(pos, srclen, dst);
		pos += dstlen;
	}
	return base;
}

static JavaVM* jvm;

static void runJVM(std::vector<std::wstring>& paths, void* pCreateJavaVM) {
	JavaVMOption options[1];
	JNIEnv* env = NULL;
	JavaVMInitArgs vm_args;
	long status;
	jclass cls;
	jmethodID mid;
	jint square;
	jboolean anot = 0;
	jobject jobj = NULL, jmanifest = NULL, jattr = NULL, jvalue;

	std::wstring jarpath = L"-Djava.class.path=";
	std::wstring jars;
	for (auto& f : paths) {
		jars = jars + (f + TEXT(PATH_SEPARATOR));
	}
	jarpath = jarpath + jars;
	auto cjarpath = toUTF8String(jarpath);
	options[0].optionString = (char*)cjarpath.c_str();
	vm_args.version = JNI_VERSION_1_8;
	vm_args.nOptions = 1;
	vm_args.options = options;
	auto CreateJavaVM = (jint(JNICALL*)(JavaVM**, void**, void*))pCreateJavaVM;
	status = CreateJavaVM(&jvm, (void**)&env, &vm_args);
	//delete [] cjarpath;
	if (status != JNI_ERR)
	{
		for (auto& jarName : paths) {
			jvm->AttachCurrentThread((void**)&env, NULL);
			// 此处利用JarFile类查找jar包中的manifest属性
			std::string mainclasspath = "";
			cls = env->FindClass("java/util/jar/JarFile");
			if (cls != 0) {
				mid = env->GetMethodID(cls, "<init>", "(Ljava/lang/String;)V");
				if (mid != 0) {
					int iSize = (int)wcslen(jarName.c_str());
					jstring dn = env->NewString((const jchar*)jarName.c_str(), iSize);
					jobj = env->NewObject(cls, mid, dn);
					jmanifest = env->CallObjectMethod(jobj, env->GetMethodID(cls, "getManifest", "()Ljava/util/jar/Manifest;"));
					cls = env->FindClass("java/util/jar/Manifest");
					if (cls != 0 && jmanifest != 0) {
						jattr = env->CallObjectMethod(jmanifest, env->GetMethodID(cls, "getMainAttributes",
							"()Ljava/util/jar/Attributes;"));
						cls = env->FindClass("java/util/jar/Attributes");
						if (cls != 0 && jattr != 0) {
							std::string maincls = "Main-Class";
							jvalue = env->CallObjectMethod(jattr, env->GetMethodID(cls, "getValue",
								"(Ljava/lang/String;)Ljava/lang/String;"), env->NewStringUTF(maincls.c_str()));
							if (jvalue != 0) {
								bool c = false;
								mainclasspath = env->GetStringUTFChars((jstring)jvalue, (jboolean*)&c);
							}
						}
					}
				}
			}
			if (checkException(env) || mainclasspath == "") {
				jvm->DetachCurrentThread();
				continue;
			}
			// 查找Main-Class结束
			replace(mainclasspath, ".", "/");
			std::wcout << L"[JR] load " << jarName << L", entry=";
			std::cout << mainclasspath << std::endl;
			cls = env->FindClass(mainclasspath.c_str());
			if (cls != 0)
			{
				mid = env->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V");
				if (mid != 0)
				{
					// 获取平台路径和版本并装入main函数参数中进行启动
					std::ifstream file;
					wchar_t curDir[MAX_PATH]{ 0 };
					GetModuleFileName(GetSelfModuleHandle(), curDir, MAX_PATH);
					int len = (int)wcslen(curDir);
					jstring dllname = env->NewString((const jchar*)curDir, len);
					len = (int)wcslen(VERSION);
					jstring version = env->NewString((const jchar*)VERSION, len);
					len = (int)wcslen(ISFORCOMMERCIAL);
					jstring commercial = env->NewString((const jchar*)ISFORCOMMERCIAL, len);
					jobjectArray a = env->NewObjectArray(3, env->FindClass("java/lang/String"), NULL);
					env->SetObjectArrayElement(a, 0, dllname);
					env->SetObjectArrayElement(a, 1, version);
					env->SetObjectArrayElement(a, 2, commercial);
					square = env->CallStaticIntMethod(cls, mid, a);
					checkException(env);
				}
			}
			jvm->DetachCurrentThread();
		}
	}
	else {
		wprintf(L"[JR] JVM create failed.\n");
	}
}



static void initJVMs() {
	SetCurrentDirectoryA(getLocalPath().c_str());
	std::string plugins = "plugins/";
	std::string settingdir = plugins + "settings/";				// 固定配置文件目录 - plugins/settings
	std::string settingpath = settingdir + "javasetting.ini";	// 固定配置文件 - javasetting.ini
	char jvmpath[MAX_PATH]{ 0 };
	auto len = GetPrivateProfileStringA("JVM", "jvmpath", NULL, jvmpath, MAX_PATH, settingpath.c_str());
	if (len < 1) {
		PR(u8"[JR] 未能读取jvm配置文件，使用默认配置[详见" + settingpath + u8"]。通常jvm位于%JRE_PATH%\\bin\\server文件夹下。");
		strcpy_s(jvmpath, "jvm.dll");		// 默认路径 - 假设环境变量Path中已包含jvm.dll
		CreateDirectoryA(plugins.c_str(), 0);
		CreateDirectoryA(settingdir.c_str(), 0);
		WritePrivateProfileStringA("JVM", "jvmpath", "jvm.dll", settingpath.c_str());
	}
	char jardir[MAX_PATH]{ 0 };
	len = GetPrivateProfileStringA("JAR", "jardir", NULL, jardir, MAX_PATH, settingpath.c_str());
	if (len < 1) {
		strcpy_s(jardir, "JAR");		// 默认目录 - JAR
		WritePrivateProfileStringA("JAR", "jardir", "JAR", settingpath.c_str());
	}
	auto hModule = LoadLibraryA(jvmpath);
	if (hModule == NULL)
	{
		PR(u8"[JR] 未能成功加载jvm.dll，请确认配置文件是否正确。[详见" + settingpath + u8"]");
		return;
	}
	auto CreateJavaVM = GetProcAddress(hModule, "JNI_CreateJavaVM");
	// 局部方法，加载所有jar插件
	{
		if (!PathIsDirectoryA(jardir)) {
			PR(u8"[JR] 未能检测到jar插件文件夹 " + std::string(jardir) + u8" 存在，请检查配置文件中路径是否正确。");
			return;
		}
		std::string pair = std::string(jardir) + "\\*.bds.jar";
		WIN32_FIND_DATAA ffd;
		HANDLE dfh = FindFirstFileA(pair.c_str(), &ffd);

		if (INVALID_HANDLE_VALUE != dfh) {
			std::vector<std::wstring> paths;
			do
			{
				if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				{
					std::string fileName = std::string(jardir) + "\\" + ffd.cFileName;
					auto w_str = toGBKString(fileName);
					paths.push_back(w_str);
				}
			} while (FindNextFileA(dfh, &ffd) != 0);
			FindClose(dfh);
			runJVM(paths, CreateJavaVM);
		}
	}
}
// 初始化工作
static void initMods() {
	if (inited) {
		return;
	}
	inited = true;
	initJVMs();
}

static void shutdownJVM() {
	if (inited) {
		if (jvm) {
			jvm->DestroyJavaVM();
			jvm = 0;
		}
		inited = false;
	}
}

// 底层相关

static std::unordered_map<void*, void**> hooks;

// 获取指定原型存储位置
void** getOriginalData(void* hook) {
	return hooks[hook];
}

// 挂载hook
HookErrorCode mTHook2(RVA sym, void* hook) {
	hooks[hook] = new void* [1]{ 0 };
	void** org = hooks[hook];
	*org = ((char*)GetModuleHandle(NULL)) + sym;
	return Hook<void*>(org, hook);
}

static bool readHardMemory(int rva, unsigned char* odata, int size) {
	//修改页都保护属性
	DWORD dwOldProtect1, dwOldProtect2 = PAGE_READONLY;
	MEMORY_BASIC_INFORMATION mbi;
	auto x = SYM_POINT(char, rva);
	SIZE_T num = 1;
	VirtualQuery(x, &mbi, sizeof(mbi));
	VirtualProtectEx(GetCurrentProcess(), x, size, dwOldProtect2, &dwOldProtect1);
	ReadProcessMemory(GetCurrentProcess(),
		x, odata, size, &num);
	//恢复页都保护属性
	return VirtualProtectEx(GetCurrentProcess(), x, size, dwOldProtect1, &dwOldProtect2);
}

static bool writeHardMemory(int rva, unsigned char* ndata, int size) {
	//修改页都保护属性
	DWORD dwOldProtect1, dwOldProtect2 = PAGE_READWRITE;
	MEMORY_BASIC_INFORMATION mbi;
	auto x = SYM_POINT(char, rva);
	SIZE_T num = 1;
	VirtualQuery(x, &mbi, sizeof(mbi));
	VirtualProtectEx(GetCurrentProcess(), x, size, dwOldProtect2, &dwOldProtect1);
	WriteProcessMemory(GetCurrentProcess(),
		x, ndata, size, &num);
	//恢复页都保护属性
	return VirtualProtectEx(GetCurrentProcess(), x, size, dwOldProtect1, &dwOldProtect2);
}

JNIEXPORT jlong JNICALL Java_BDS_MCJAVAAPI_dlsym
(JNIEnv* ev, jobject api, jint rva) {
	return (VA)GetModuleHandle(NULL) + rva;
}
JNIEXPORT jbyteArray JNICALL Java_BDS_MCJAVAAPI_readHardMemory
(JNIEnv* ev, jobject api, jint rva, jint size) {
	if (size < 1)
		return 0;
	char *buf = new char[size];
	readHardMemory(rva, (unsigned char*)buf, size);
	auto ja = ev->NewByteArray(size);
	ev->SetByteArrayRegion(ja, 0, size, (const jbyte*)buf);
	delete []buf;
	return ja;
}
JNIEXPORT jboolean JNICALL Java_BDS_MCJAVAAPI_writeHardMemory
(JNIEnv* ev, jobject api, jint rva, jbyteArray ja, jint size) {
	if (size < 1)
		return false;
	jboolean c = false;
	auto data = ev->GetByteArrayElements(ja, &c);
	bool ret = writeHardMemory(rva, (unsigned char*)data, size);
	return ret;
}

void init() {
	// 此处填写插件加载时的操作
	std::cout << u8"{[插件] Java插件运行平台（社区版）已装载。此平台基于LGPL协议发行。" << std::endl;
	std::wcout << L"version=" << VERSION << std::endl;
}

void exit() {
	// 此处填写插件卸载时的操作
}
