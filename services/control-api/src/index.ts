import Fastify from "fastify";

const app = Fastify({ logger: true });
const port = Number(process.env.PORT || 8080);

app.get("/health", async () => ({ status: "ok", service: "control-api" }));

app.listen({ host: "0.0.0.0", port }).catch((error) => {
  app.log.error(error);
  process.exit(1);
});
