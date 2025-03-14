# cppthreadpool

## 一、简介
`cppthreadpool`是一个用C++11编写的header-only线程池库，具备任务提交、结果获取、回调处理以及动态线程管理等功能，实现优雅便捷。

## 二、功能特点
1. **任务提交简洁**：支持不带回调和带回调两种方式提交任务。
2. **返回值获取**：通过`std::shared_future`获取任务执行后的返回值 。
3. **动态线程管理**：根据任务负载动态调整线程数量。


## 三、使用方法
### （一）安装
1. 将本项目仓库克隆到本地。
2. 在你的项目中包含`cppthreadpool.hpp`。


### （二）示例代码

#### 1. 不使用回调提交任务
```cpp
int main()
{
    // 线程数量4-8
    cppthreadpool::ThreadPool pool(4, 8);
    auto result = pool.submit([]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        return 42;
    });
    if (result.success)
    {
        std::cout << "Task result: " << result.future.get() << std::endl;
    }
    else
    {
        std::cout << "Task submission failed" << std::endl;
    }
    pool.shutdown();
    return 0;
}
```
#### 2. 配合回调提交任务
**tip：callback形参数可接收来自task的返回数据,并在task所在线程进行处理**
```cpp
void callback(int value)
{
    std::cout << "Callback received value: " << value << std::endl;
}

int main()
{
    // 线程数量 4-4
    cppthreadpool::ThreadPool pool(4);
    auto result = pool.submit([]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        // return data to callback fun
        return 42;
    }, callback);
    if (result.success)
    {
        std::cout << "Task submitted successfully" << std::endl;
    }
    else
    {
        std::cout << "Task submission failed" << std::endl;
    }
    pool.shutdown();
    return 0;
}
```
### 引用捕获和与引用传参
**Tip**: 当回调函数接受来自task的引用类型时，需要显式指定task返回值类型以用于模板类型推导。

    Example of task that returns a reference to a callback:
        1. string& fun(string& msg) { return msg; }
        2. [](string& msg) ->string& { return msg; }

```cpp
void printMessage(const std::string& message) 
{
     std::cout << "Message: " << message << std::endl;
}

int main() 
{
    std::string message = "Hello from thread pool!";

    printMessage(message);

    cppthreadpool::ThreadPool pool(4);
    // task or callback 使用 [] 捕获引用
    auto result = pool.submit([&message]() 
    {
        message = "Modified in lambda";
    });

    if (result.success)
    {
        // get return from lambda
        std::string lambdaMessage = result.future.get();
        std::cout << "Task completed successfully";
        if(lambdaMessage==message)
            printMessage(message);
    }    
    else
        std::cout << "Task submission failed" << std::endl;

    // 使用 std::ref 传递引用
    auto refResult = pool.submit([](std::string& msg) 
    {
        msg = "Modified in lambda with std::ref";
    }, std::ref(message));

    
    if (refResult.success)
    {
        refResult.future.wait();
        std::cout << "Task with std::ref completed successfully" << std::endl;
        printMessage(message);
    }
    else
        std::cout << "Task with std::ref submission failed" << std::endl;

    pool.shutdown();
    return 0;

    // callback获取task返回的引用
    auto callbackResult = pool.submit([](std::string& msg)->std::string &
    {
        return msg;
    },[](std::string &msg)
    {
        msg = "Modified in lambda with callback";
    }
    , std::ref(message));

    
    if (callbackResult.success)
    {
        callbackResult.future.wait();
        std::cout << "Task with callbackResult completed successfully" << std::endl;
        printMessage(message);
    }
    else
        std::cout << "Task with callbackResult submission failed" << std::endl;

    pool.shutdown();
    return 0;
}
```

## 四、注意事项
- **异常处理**：任务执行过程中若发生异常，会在控制台输出错误信息。
- **线程安全**：虽然线程池自身具备线程安全性，但在任务函数中涉及共享资源时，仍需自行处理同步问题。

## 五、贡献
欢迎大家提交代码、反馈问题或提出建议，共同完善这个线程池库。

## 六、许可证
本项目遵循 MIT 许可证。
