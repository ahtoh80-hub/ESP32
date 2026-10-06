using System;
using System.Text;

namespace MyHome
{
    class Program
    {
        static void Main(string[] args)
        {
            // Устанавливаем UTF-8 кодировку для корректного отображения символов
            Console.OutputEncoding = Encoding.UTF8;
            Console.InputEncoding = Encoding.UTF8;

            // Выводим котика символами ASCII-арта
            Console.WriteLine();
            Console.WriteLine("        \\     /");
            Console.WriteLine("         \\   /");
            Console.WriteLine("          \\_/    ____");
            Console.WriteLine("           \\   /    \\");
            Console.WriteLine("            \\_/  _  \\");
            Console.WriteLine("             \\   \\  \\___");
            Console.WriteLine("              \\    \\__   \\");
            Console.WriteLine("               \\   /  \\   \\");
            Console.WriteLine("                \\_/    \\   \\");
            Console.WriteLine("                      \\   \\");
            Console.WriteLine("                       \\   \\");
            Console.WriteLine("                        \\___\\");
            Console.WriteLine();
            Console.WriteLine("    Привет, я котик! =^..^=");
            Console.WriteLine();
            Console.WriteLine("    ╔══════════════════════════════╗");
            Console.WriteLine("    ║   Добро пожаловать в MyHome!  ║");
            Console.WriteLine("    ╚══════════════════════════════╝");
            Console.WriteLine();
        }
    }
}