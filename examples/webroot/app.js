document.getElementById("go").addEventListener("click", async () => {
  const name = document.getElementById("name").value || "world";
  const response = await fetch("/api/hello/" + encodeURIComponent(name));
  const data = await response.json();
  document.getElementById("out").textContent = JSON.stringify(data, null, 2);
});
