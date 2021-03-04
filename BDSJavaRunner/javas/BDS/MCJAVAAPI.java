package BDS;

/**
 * API接口定义
 * @author BDSJavaRunner
 */
public class MCJAVAAPI {
	
	private String mVersion;
	private boolean mcommercial;
	
	public MCJAVAAPI(String DLLPath, String ver, String commercial) {
		mVersion = ver;
		mcommercial = commercial == "1";
        initApis(DLLPath);
	}
	
	private void initApis(String DLLPath) {
		System.load(DLLPath);
	}
	public String getVersion() {
		return mVersion;
	}
	public boolean isCommercial() {
		return mcommercial;
	}

	/**
	 * 事件处理接口
	 */
	public interface EventCab {
		/**
		 * 回调函数关键字
		 * @param data - 事件数据
		 * @return 是否继续/拦截(before事件有效)
		 */
		boolean callback(String data);
	}
	/**
	 * 置入tick主线程的代码段接口
	 */
	public interface TickFunc {
		/**
		 * 待置入的代码段实现
		 */
		void callback();
	}
	/**
	 * 官方脚本引擎执行结果回调接口
	 */
	public interface JSECab {
		/**
		 * 回调函数关键字
		 * @param r - 是否执行成功
		 */
		void callback(boolean r);
	}

	//////////////// native方法声明 ////////////////
	
	/**
	 * 标准输出流打印消息
	 * @param log - 待输出至标准流字符串
	 */
	native public void log(String log);
	/**
	 * 添加事件加载前监听器
	 * @param eventkey - 注册用关键字
	 * @param cb - 供事件触发时的回调函数对象
	 * @return
	 */
	native public boolean addBeforeActListener(String eventkey, EventCab cb);
	/**
	 * 添加事件加载后监听器
	 * @param eventkey - 注册用关键字
	 * @param cb - 供事件触发时的回调函数对象
	 * @return
	 */
	native public boolean addAfterActListener(String eventkey, EventCab cb);
	/**
	 * 移除事件加载前监听器
	 * @param eventkey - 注册用关键字
	 * @param cb - 待移除的回调函数对象
	 * @return 是否移除成功
	 */
	native public boolean removeBeforeActListener(String eventkey, EventCab cb);
	/**
	 * 移除事件加载后监听器
	 * @param eventkey - 注册用关键字
	 * @param cb - 待移除的回调函数对象
	 * @return 是否移除成功
	 */
	native public boolean removeAfterActListener(String eventkey, EventCab cb);
	/**
	 * 发送一个函数至下一tick运行
	 * @param f - 待置入下一tick执行的函数
	 */
	native public void postTick(TickFunc f);
	/**
	 * 设置一个全局指令说明<br/>
	 * （备注：延期注册的情况，可能不会改变客户端界面）
	 * @param cmd - 命令
	 * @param description - 命令说明
	 */
	native public void setCommandDescribe(String cmd, String description);
	/**
	 * 执行后台指令
	 * @param cmd - 语法正确的MC指令
	 * @return 是否正常执行
	 */
	native public boolean runcmd(String cmd);
	/**
	 * 发送一条命令输出消息（可被拦截）
	 * @param cmdout - 待发送的命令输出字符串
	 */
	native public void logout(String cmdout);
	/**
	 * 获取在线玩家列表
	 * @return 玩家列表的Json字符串
	 */
	native public String getOnLinePlayers();
	/**
	 * 使用官方脚本引擎新增一段行为包脚本并执行
	 * @param js - 脚本文本
	 * @param cb - 执行结果回调
	 */
	native public void JSErunScript(String js, JSECab cb);
	/**
	 * 使用官方脚本引擎发送一个自定义事件广播
	 * @param ename - 自定义事件名称（不能以minecraft:开头）
	 * @param data - 事件内容文本
	 * @param cb - 执行结果回调
	 */
	native public void JSEfireCustomEvent(String ename, String data, JSECab cb);
	/**
	 * 重命名一个指定的玩家名<br/>
	 * （备注：该函数可能不会变更客户端实际显示名）
	 * @param uuid - 在线玩家的uuid字符串
	 * @param newName - 新的名称
	 * @return 是否命名成功
	 */
	native public boolean reNameByUuid(String uuid, String newName);
	/**
	 * 增加玩家一个物品
	 * @param uuid - 在线玩家的uuid字符串
	 * @param id - 物品id值
	 * @param aux - 物品特殊值
	 * @param count - 数量
	 * @return
	 */
	native public boolean addPlayerItem(String uuid, int id, short aux, byte count);
	/**
	 * 查询在线玩家基本信息
	 * @param uuid - 在线玩家的uuid字符串
	 * @return 玩家基本信息json字符串
	 */
	native public String selectPlayer(String uuid);
	/**
	 * 模拟玩家发送一个文本
	 * @param uuid - 在线玩家的uuid字符串
	 * @param msg - 待模拟发送的文本
	 * @return 是否发送成功
	 */
	native public boolean talkAs(String uuid, String msg);
	/**
	 * 模拟玩家执行一个指令
	 * @param uuid - 在线玩家的uuid字符串
	 * @param cmd - 待模拟执行的指令
	 * @return 是否发送成功
	 */
	native public boolean runcmdAs(String uuid, String cmd);
	/**
	 * 断开一个玩家的连接
	 * @param uuid - 在线玩家的uuid字符串
	 * @param tips - 断开提示（设空值则为默认值）
	 * @return 是否成功断开连接
	 */
	native public boolean disconnectClient(String uuid, String tips);
	/**
	 * 发送一个原始显示文本给玩家
	 * @param uuid - 在线玩家的uuid字符串
	 * @param msg - 文本内容，空白内容则不予发送
	 * @return 是否发送成功
	 */
	native public boolean sendText(String uuid, String msg);
	/**
	 * 向指定的玩家发送一个简单表单
	 * @param uuid - 在线玩家的uuid字符串
	 * @param title - 表单标题
	 * @param content - 内容
	 * @param buttons - 按钮文本数组字符串
	 * @return 创建的表单id，为 0 表示发送失败
	 */
	native public int sendSimpleForm(String uuid, String title, String content, String buttons);
	/**
	 * 向指定的玩家发送一个模式对话框
	 * @param uuid - 在线玩家的uuid字符串
	 * @param title - 表单标题
	 * @param content - 内容
	 * @param button1 - 按钮1标题（点击该按钮selected为true）
	 * @param button2 - 按钮2标题（点击该按钮selected为false）
	 * @return 创建的表单id，为 0 表示发送失败
	 */
	native public int sendModalForm(String uuid, String title, String content, String button1, String button2);
	/**
	 * 向指定的玩家发送一个自定义表单
	 * @param uuid - 在线玩家的uuid字符串
	 * @param json - 自定义表单的json字符串（要使用自定义表单类型，参考nk、pm格式或minebbs专栏）
	 * @return 创建的表单id，为 0 表示发送失败
	 */
	native public int sendCustomForm(String uuid, String json);
	/**
	 * 放弃一个表单<br/>
	 * （备注：已被接收到的表单会被自动释放）
	 * @param formid - 表单id
	 * @return 是否释放成功
	 */
	native public boolean releaseForm(int formid);
	/**
	 * 获取指定玩家指定计分板上的数值
	 * @param uuid - 在线玩家的uuid字符串
	 * @param objname - 计分板登记的名称
	 * @return 获取的目标值，若目标不存在则返回0
	 */
	native public int getscoreboard(String uuid, String objname);
	/**
	 * 设置指定玩家指定计分板上的数值<br/>
	 * （注：特定情况下会自动创建计分板）
	 * @param uuid - 在线玩家的uuid字符串
	 * @param objname - 计分板登记的名称
	 * @param count - 待设定的目标值
	 * @return 是否设置成功
	 */
	native public boolean setscoreboard(String uuid, String objname, int count);
	/**
	 * 设置服务器的显示名信息<br/>
	 * （备注：服务器名称加载时机在地图完成载入之后）
	 * @param motd - 新服务器显示名信息
	 * @param isShow - 是否公开显示
	 * @return 是否设置成功
	 */
	native public boolean setServerMotd(String motd, boolean isShow);
	/**
	 * 获取指定ID对应于计分板上的数值
	 * @param id - 离线计分板的ID
	 * @param objname - 计分板登记的名称（若不存在则自动添加）
	 * @return 获取的目标值，若目标不存在则返回0
	 */
	native public int getscoreById(long id, String objname);
	/**
	 * 设置指定ID对应于计分板上的数值
	 * @param id - 离线计分板的ID
	 * @param objname - 计分板登记的名称（若不存在则自动添加）
	 * @param count - 待设置的值
	 * @return 设置后的目标值，若未成功则返回0
	 */
	native public int setscoreById(long id, String objname, int count);
	/**
	 * 取相对地址对应的实际指针
	 * @param rva - 函数起始位置相对地址
	 * @return
	 */
	native public long dlsym(int rva);
	/**
	 * 读特定段内存硬编码
	 * @param rva - 函数片段起始位置相对地址
	 * @param size - 内存长度
	 * @return 数据内容
	 */
	native public byte[] readHardMemory(int rva, int size);
	/**
	 * 写特定段内存硬编码
	 * @param rva - 函数片段起始位置相对地址
	 * @param data - 新数据内容
	 * @param size - 内存长度
	 * @return 是否操作成功
	 */
	native public boolean writeHardMemory(int rva, byte[] data, int size);
}
