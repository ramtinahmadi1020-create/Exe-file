using System.ComponentModel;
using System.Runtime.CompilerServices;
using Brush = System.Windows.Media.Brush;
using SolidColorBrush = System.Windows.Media.SolidColorBrush;
using BrushConverter = System.Windows.Media.BrushConverter;

namespace VpnEcho;

public enum ProbeState { Pending, Checking, Ok, Failed }

public sealed class ServerEntry : INotifyPropertyChanged
{
    private ProbeState _state;
    private double _ms, _score;
    private int _samples;
    private string _error = "";
    private bool _active;
    private string _resolvedIp = "";
    private static readonly Brush Muted = Make("#899196"), Green = Make("#66C995"), Red = Make("#DC7E7E"), Card = Make("#1D2124"), ActiveCard = Make("#252D2D");
    public ServerProfile Profile { get; }
    public int Id => Profile.Id;
    public string Name => Profile.Name;
    public string Address => Profile.Address;
    public ProbeState State => _state;
    public double Ms => _ms;
    public double Score => _score;
    public int Samples => _samples;
    public string Error => _error;
    public string ResolvedIp { get => _resolvedIp; set { _resolvedIp = value; OnPropertyChanged(); } }
    public bool IsActive { get => _active; set { _active = value; NotifyDisplay(); } }
    public string Mark => IsActive ? "●" : "○";
    public Brush MarkBrush => IsActive ? Green : Muted;
    public Brush RowBrush => IsActive ? ActiveCard : Card;
    public string ValueText => State switch { ProbeState.Checking => "Checking…", ProbeState.Failed => "Unavailable", ProbeState.Ok => $"{Ms:0} ms", _ => "—" };
    public Brush ValueBrush => State switch { ProbeState.Failed => Red, ProbeState.Ok when Ms < 1000 => Green, ProbeState.Ok => Red, _ => Muted };
    public string Subtitle => IsActive ? "Active connection" : State switch
    {
        ProbeState.Checking => "Testing the VMess connection",
        ProbeState.Ok => Samples > 1 ? "Verified · 2 samples" : "Verified · 1 sample",
        ProbeState.Failed => "No verified response · tap for info",
        _ => "Not tested yet"
    };
    public event PropertyChangedEventHandler? PropertyChanged;
    public ServerEntry(ServerProfile profile) => Profile = profile;
    public void SetPending() { _state = ProbeState.Pending; _ms = _score = 0; _samples = 0; _error = ""; NotifyDisplay(); }
    public void SetChecking() { _state = ProbeState.Checking; _ms = _score = 0; _samples = 0; _error = ""; NotifyDisplay(); }
    public void SetSuccess(double ms, int samples, double score, string ip) { _state = ProbeState.Ok; _ms = ms; _score = score; _samples = samples; _error = ""; _resolvedIp = ip; NotifyDisplay(); }
    public void SetFailure(string error) { _state = ProbeState.Failed; _error = error; NotifyDisplay(); }
    private void NotifyDisplay() { OnPropertyChanged(nameof(State)); OnPropertyChanged(nameof(Ms)); OnPropertyChanged(nameof(Score)); OnPropertyChanged(nameof(Samples)); OnPropertyChanged(nameof(Error)); OnPropertyChanged(nameof(IsActive)); OnPropertyChanged(nameof(Mark)); OnPropertyChanged(nameof(MarkBrush)); OnPropertyChanged(nameof(RowBrush)); OnPropertyChanged(nameof(ValueText)); OnPropertyChanged(nameof(ValueBrush)); OnPropertyChanged(nameof(Subtitle)); }
    private void OnPropertyChanged([CallerMemberName] string? name = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    private static Brush Make(string color) { var b = (SolidColorBrush)new BrushConverter().ConvertFromString(color)!; b.Freeze(); return b; }
}

public sealed record ProbeResult(bool Success, double AverageMs, double Score, int Samples, string ResolvedIp, string Error)
{
    public static ProbeResult Fail(string message, string ip = "") => new(false, 0, 0, 0, ip, message);
}
