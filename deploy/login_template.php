<?php
$pw_hash     = '__HASH__';
$salt        = '__SALT__';
$cookie_name = 'loen_auth';
$cookie_ttl  = 365 * 24 * 3600;
$valid_token = hash_hmac('sha256', 'loen_v1', $pw_hash);

if (isset($_GET['logout'])) {
    setcookie($cookie_name, '', time() - 3600, '/');
    header('Location: /');
    exit;
}

$error = false;
if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['password'])) {
    $attempt = hash_pbkdf2('sha256', $_POST['password'], $salt, 100000);
    if (hash_equals($pw_hash, $attempt)) {
        setcookie($cookie_name, $valid_token, time() + $cookie_ttl, '/', '', false, true);
        header('Location: ' . strtok($_SERVER['REQUEST_URI'], '?'));
        exit;
    }
    $error = true;
}

if (!isset($_COOKIE[$cookie_name]) || !hash_equals($valid_token, $_COOKIE[$cookie_name])) { ?>
<!DOCTYPE html>
<html lang="da">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lønoversigt</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:#f0f2f5;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:1rem}
.card{background:#fff;border-radius:14px;box-shadow:0 4px 24px rgba(0,0,0,.10);padding:2rem 2rem 2.5rem;width:100%;max-width:360px}
.brand{background:#1a237e;border-radius:10px;padding:1.2rem 1rem;text-align:center;margin-bottom:1.75rem}
.brand h1{color:#fff;font-size:1.25rem;font-weight:700;letter-spacing:-.01em}
.brand p{color:rgba(255,255,255,.6);font-size:.8rem;margin-top:.2rem}
label{display:block;font-size:.85rem;font-weight:600;color:#444;margin-bottom:.5rem}
input[type=password]{display:block;width:100%;padding:.85rem 1rem;border:2px solid #e0e0e0;border-radius:9px;font-size:1rem;outline:none;transition:border-color .15s}
input[type=password]:focus{border-color:#1a237e}
.err{background:#fce4ec;color:#b71c1c;border-radius:7px;padding:.65rem 1rem;font-size:.85rem;margin-bottom:1rem}
button{display:block;width:100%;margin-top:1.1rem;padding:.9rem;background:#1a237e;color:#fff;border:none;border-radius:9px;font-size:1rem;font-weight:600;cursor:pointer;transition:background .15s}
button:hover{background:#283593}
button:active{background:#0d1757}
</style>
</head>
<body>
<div class="card">
  <div class="brand">
    <h1>Lønoversigt</h1>
    <p>emandersen.dk</p>
  </div>
  <?php if ($error): ?>
  <div class="err">Forkert adgangskode — prøv igen</div>
  <?php endif; ?>
  <form method="post" autocomplete="on">
    <label for="pw">Adgangskode</label>
    <input type="password" id="pw" name="password"
           autocomplete="current-password"
           autofocus placeholder="••••••••">
    <button type="submit">Log ind</button>
  </form>
</div>
</body>
</html>
<?php
    exit;
}

readfile(__DIR__ . '/report.html');
