using System;
public class P {
  public static void Main(){
    Console.WriteLine("HELLO from .NET on device: " + System.Runtime.InteropServices.RuntimeInformation.OSArchitecture);
    Console.WriteLine("kernel: " + Environment.OSVersion);
    Console.WriteLine("CWD: " + Environment.CurrentDirectory);
  }
}
