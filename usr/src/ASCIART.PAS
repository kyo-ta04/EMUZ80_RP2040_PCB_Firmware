program asciart;

label break;

var
  i,x,y: integer;
  a,b,ca,cb,t: real;
  h: string[18];
  junk: string[255];

begin
  write('Hit Enter key:');
  readln(junk);
  h := '0123456789ABCDEF ';

  for y:=-12 to 12 do begin
    for x:= -39 to 39 do begin
      ca := x * 0.0458;
      cb := y * 0.08333;
      a := ca;
      b := cb;
      i := 0;
      while (i <= 15) do begin
        t := a * a - b * b + ca;
        b := 2 * a * b + cb;
        a := t;
        if ((a * a + b * b) > 4) then
          goto break;
        i := i + 1;
      end;
break:
      write(h[i+1]);
    end;
    writeln('');
  end;
  writeln('OK');
  writeln('Hit Enter key:');
  readln(junk);
end.
